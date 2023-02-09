#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// TEMP: add the proper automake stuff
#define HAVE_NANOSLEEP
#define HAVE_UNISTD_H

#include "dfu.h"
#include "dfu_util.h"
#include "portable.h"
#include <libusb.h>

#include "json.hpp"

#include <atomic>
#include <iostream>
#include <string>
#include <thread>

/*
 * Look for a descriptor in a concatenated descriptor list. Will
 * return upon the first match of the given descriptor type. Returns length of
 * found descriptor, limited to res_size
 */
static int find_descriptor(const uint8_t *desc_list, int list_len,
                           uint8_t desc_type, void *res_buf, int res_size) {
  int p = 0;

  if (list_len < 2)
    return (-1);

  while (p + 1 < list_len) {
    int desclen;

    desclen = (int)desc_list[p];
    if (desclen == 0) {
      warnx("Invalid descriptor list");
      return -1;
    }
    if (desc_list[p + 1] == desc_type) {
      if (desclen > res_size)
        desclen = res_size;
      if (p + desclen > list_len)
        desclen = list_len - p;
      memcpy(res_buf, &desc_list[p], desclen);
      return desclen;
    }
    p += (int)desc_list[p];
  }
  return -1;
}

/*
 * Similar to libusb_get_string_descriptor_ascii but will allow
 * truncated descriptors (descriptor length mismatch) seen on
 * e.g. the STM32F427 ROM bootloader.
 */
static int get_string_descriptor_ascii(libusb_device_handle *devh,
                                       uint8_t desc_index, unsigned char *data,
                                       int length) {
  unsigned char tbuf[255];
  uint16_t langid;
  int r, di, si;

  /* get the language IDs and pick the first one */
  r = libusb_get_string_descriptor(devh, 0, 0, tbuf, sizeof(tbuf));
  if (r < 0) {
    warnx("Failed to retrieve language identifiers");
    return r;
  }
  if (r < 4 || tbuf[0] < 4 ||
      tbuf[1] != LIBUSB_DT_STRING) { /* must have at least one ID */
    warnx("Broken LANGID string descriptor");
    return -1;
  }
  langid = tbuf[2] | (tbuf[3] << 8);

  r = libusb_get_string_descriptor(devh, desc_index, langid, tbuf,
                                   sizeof(tbuf));
  if (r < 0) {
    warnx("Failed to retrieve string descriptor %d", desc_index);
    return r;
  }
  if (tbuf[1] != LIBUSB_DT_STRING) { /* sanity check */
    warnx("Malformed string descriptor %d, type = 0x%02x", desc_index, tbuf[1]);
    return -1;
  }
  if (tbuf[0] > r) { /* if short read,           */
    warnx("Patching string descriptor %d length (was %d, received %d)",
          desc_index, tbuf[0], r);
    tbuf[0] = r; /* fix up descriptor length */
  }

  /* convert from 16-bit unicode to ascii string */
  for (di = 0, si = 2; si + 1 < tbuf[0] && di < length; si += 2) {
    if (tbuf[si + 1]) /* high byte of unicode char */
      data[di++] = '?';
    else
      data[di++] = tbuf[si];
  }
  data[di] = 0;
  return di;
}

static void probe_configuration(libusb_device *dev,
                                struct libusb_device_descriptor *desc) {
  struct usb_dfu_func_descriptor func_dfu;
  libusb_device_handle *devh;
  struct dfu_if *pdfu;
  struct libusb_config_descriptor *cfg;
  const struct libusb_interface_descriptor *intf;
  const struct libusb_interface *uif;
  char alt_name[MAX_DESC_STR_LEN + 1];
  char serial_name[MAX_DESC_STR_LEN + 1];
  int cfg_idx;
  int intf_idx;
  int alt_idx;
  int ret;
  int has_dfu;

  for (cfg_idx = 0; cfg_idx != desc->bNumConfigurations; cfg_idx++) {
    memset(&func_dfu, 0, sizeof(func_dfu));
    has_dfu = 0;

    ret = libusb_get_config_descriptor(dev, cfg_idx, &cfg);
    if (ret != 0)
      return;
    if (match_config_index > -1 &&
        match_config_index != cfg->bConfigurationValue) {
      libusb_free_config_descriptor(cfg);
      continue;
    }

    /*
     * In some cases, noticably FreeBSD if uid != 0,
     * the configuration descriptors are empty
     */
    if (!cfg)
      return;

    ret = find_descriptor(cfg->extra, cfg->extra_length, USB_DT_DFU, &func_dfu,
                          sizeof(func_dfu));
    if (ret > -1)
      goto found_dfu;

    for (intf_idx = 0; intf_idx < cfg->bNumInterfaces; intf_idx++) {
      uif = &cfg->interface[intf_idx];
      if (!uif)
        break;

      for (alt_idx = 0; alt_idx < cfg->interface[intf_idx].num_altsetting;
           alt_idx++) {
        intf = &uif->altsetting[alt_idx];

        if (intf->bInterfaceClass != 0xfe || intf->bInterfaceSubClass != 1)
          continue;

        ret = find_descriptor(intf->extra, intf->extra_length, USB_DT_DFU,
                              &func_dfu, sizeof(func_dfu));
        if (ret > -1)
          goto found_dfu;

        has_dfu = 1;
      }
    }
    if (has_dfu) {
      /*
       * Finally try to retrieve it requesting the
       * device directly This is not supported on
       * all devices for non-standard types
       */
      if (libusb_open(dev, &devh) == 0) {
        ret = libusb_get_descriptor(devh, USB_DT_DFU, 0, (unsigned char *)&func_dfu,
                                    sizeof(func_dfu));
        libusb_close(devh);
        if (ret > -1)
          goto found_dfu;
      }
      warnx("Device has DFU interface, "
            "but has no DFU functional descriptor");

      /* fake version 1.0 */
      func_dfu.bLength = 7;
      func_dfu.bcdDFUVersion = libusb_cpu_to_le16(0x0100);
      goto found_dfu;
    }
    libusb_free_config_descriptor(cfg);
    continue;

  found_dfu:
    if (func_dfu.bLength == 7) {
      printf("Deducing device DFU version from functional descriptor "
             "length\n");
      func_dfu.bcdDFUVersion = libusb_cpu_to_le16(0x0100);
    } else if (func_dfu.bLength < 9) {
      printf("Error obtaining DFU functional descriptor\n");
      printf("Please report this as a bug!\n");
      printf("Warning: Assuming DFU version 1.0\n");
      func_dfu.bcdDFUVersion = libusb_cpu_to_le16(0x0100);
      printf("Warning: Transfer size can not be detected\n");
      func_dfu.wTransferSize = 0;
    }

    for (intf_idx = 0; intf_idx < cfg->bNumInterfaces; intf_idx++) {
      if (match_iface_index > -1 && match_iface_index != intf_idx)
        continue;

      uif = &cfg->interface[intf_idx];
      if (!uif)
        break;

      for (alt_idx = 0; alt_idx < uif->num_altsetting; alt_idx++) {
        int dfu_mode;

        intf = &uif->altsetting[alt_idx];

        if (intf->bInterfaceClass != 0xfe || intf->bInterfaceSubClass != 1)
          continue;

        dfu_mode = (intf->bInterfaceProtocol == 2);
        /* e.g. DSO Nano has bInterfaceProtocol 0 instead of 2 */
        if (func_dfu.bcdDFUVersion == 0x011a && intf->bInterfaceProtocol == 0)
          dfu_mode = 1;

        /* LPC DFU bootloader has bInterfaceProtocol 1 (Runtime) instead of 2 */
        if (desc->idVendor == 0x1fc9 && desc->idProduct == 0x000c &&
            intf->bInterfaceProtocol == 1)
          dfu_mode = 1;

        /*
         * Old Jabra devices may have bInterfaceProtocol 0 instead of 2.
         * Also runtime PID and DFU pid are the same.
         * In DFU mode, the configuration descriptor has only 1 interface.
         */
        if (desc->idVendor == 0x0b0e && intf->bInterfaceProtocol == 0 &&
            cfg->bNumInterfaces == 1)
          dfu_mode = 1;

        if (dfu_mode && match_iface_alt_index > -1 &&
            match_iface_alt_index != intf->bAlternateSetting)
          continue;

        if (dfu_mode) {
          if ((match_vendor_dfu >= 0 && match_vendor_dfu != desc->idVendor) ||
              (match_product_dfu >= 0 &&
               match_product_dfu != desc->idProduct)) {
            continue;
          }
        } else {
          if ((match_vendor >= 0 && match_vendor != desc->idVendor) ||
              (match_product >= 0 && match_product != desc->idProduct)) {
            continue;
          }
        }

        if (libusb_open(dev, &devh)) {
          warnx("Cannot open DFU device %04x:%04x", desc->idVendor,
                desc->idProduct);
          break;
        }
        if (intf->iInterface != 0)
          ret = get_string_descriptor_ascii(devh, intf->iInterface,
                                            (unsigned char *)alt_name, MAX_DESC_STR_LEN);
        else
          ret = -1;
        if (ret < 1)
          strcpy(alt_name, "UNKNOWN");
        if (desc->iSerialNumber != 0)
          ret = get_string_descriptor_ascii(
              devh, desc->iSerialNumber, (unsigned char *)serial_name, MAX_DESC_STR_LEN);
        else
          ret = -1;
        if (ret < 1)
          strcpy(serial_name, "UNKNOWN");
        libusb_close(devh);

        if (dfu_mode && match_iface_alt_name != NULL &&
            strcmp(alt_name, match_iface_alt_name))
          continue;

        if (dfu_mode) {
          if (match_serial_dfu != NULL && strcmp(match_serial_dfu, serial_name))
            continue;
        } else {
          if (match_serial != NULL && strcmp(match_serial, serial_name))
            continue;
        }

        pdfu = (dfu_if *)malloc(sizeof(*pdfu));

        memset(pdfu, 0, sizeof(*pdfu));

        pdfu->func_dfu = func_dfu;
        pdfu->dev = libusb_ref_device(dev);
        // pdfu->quirks = get_quirks(desc->idVendor,
        //     desc->idProduct, desc->bcdDevice);
        pdfu->vendor = desc->idVendor;
        pdfu->product = desc->idProduct;
        pdfu->bcdDevice = desc->bcdDevice;
        pdfu->configuration = cfg->bConfigurationValue;
        pdfu->interface = intf->bInterfaceNumber;
        pdfu->altsetting = intf->bAlternateSetting;
        pdfu->devnum = libusb_get_device_address(dev);
        pdfu->busnum = libusb_get_bus_number(dev);
        pdfu->alt_name = strdup(alt_name);
        if (pdfu->alt_name == NULL)
          errx(EX_SOFTWARE, "Out of memory");
        pdfu->serial_name = strdup(serial_name);
        if (pdfu->serial_name == NULL)
          errx(EX_SOFTWARE, "Out of memory");
        if (dfu_mode)
          pdfu->flags |= DFU_IFF_DFU;
        // if (pdfu->quirks & QUIRK_FORCE_DFU11) {
        //	pdfu->func_dfu.bcdDFUVersion =
        //	  libusb_cpu_to_le16(0x0110);
        // }
        pdfu->bMaxPacketSize0 = desc->bMaxPacketSize0;

        /* queue into list */
        pdfu->next = dfu_root;
        dfu_root = pdfu;
      }
    }
    libusb_free_config_descriptor(cfg);
  }
}

#define MAX_PATH_LEN 20
char path_buf[MAX_PATH_LEN];

char *get_path(libusb_device *dev) {
#if (defined(LIBUSB_API_VERSION) && LIBUSB_API_VERSION >= 0x01000102) ||       \
    (defined(LIBUSBX_API_VERSION) && LIBUSBX_API_VERSION >= 0x01000102)
  uint8_t path[8];
  int r, j;
  r = libusb_get_port_numbers(dev, path, sizeof(path));
  if (r > 0) {
    snprintf(path_buf, MAX_PATH_LEN, "%d-%d", libusb_get_bus_number(dev), path[0]);
    for (j = 1; j < r; j++) {
      snprintf(path_buf + strlen(path_buf), MAX_PATH_LEN, ".%d", path[j]);
    };
  }
  return path_buf;
#else
#warning "libusb too old - building without USB path support!"
  (void)dev;
  return NULL;
#endif
}

void probe_devices(libusb_context *ctx) {
  dfu_root = NULL;
  libusb_device **list;
  ssize_t num_devs;
  ssize_t i;

  num_devs = libusb_get_device_list(ctx, &list);
  for (i = 0; i < num_devs; ++i) {
    struct libusb_device_descriptor desc;
    struct libusb_device *dev = list[i];

    if (match_path != NULL && strcmp(get_path(dev), match_path) != 0)
      continue;
    if (libusb_get_device_descriptor(dev, &desc))
      continue;
    probe_configuration(dev, &desc);
  }
  libusb_free_device_list(list, 0);
}

void print_dfu_if(struct dfu_if *dfu_if) {
  printf("Found %s: [%04x:%04x] ver=%04x, devnum=%u, cfg=%u, intf=%u, "
         "path=\"%s\", alt=%u, name=\"%s\", serial=\"%s\"\n",
         dfu_if->flags & DFU_IFF_DFU ? "DFU" : "Runtime", dfu_if->vendor,
         dfu_if->product, dfu_if->bcdDevice, dfu_if->devnum,
         dfu_if->configuration, dfu_if->interface, get_path(dfu_if->dev),
         dfu_if->altsetting, dfu_if->alt_name, dfu_if->serial_name);
}

using json = nlohmann::json;
using namespace std;

std::string print_hex(int number) {
  char tmp[10];
  snprintf(tmp, 10, "0x%04x", number);
  std::string s(tmp);
  return s;
}

/* Walk the device tree and print out DFU devices */
json list_dfu_interfaces(void) {
  struct dfu_if *pdfu;

  json j;
  j["event"] = "list";

  int index = 0;

  for (pdfu = dfu_root; pdfu != NULL; pdfu = pdfu->next) {

	if (!(pdfu->flags & DFU_IFF_DFU)) {
		// decide what to do with DFU runtime objects
		// for the time being, ignore them
		continue;
	}

	for (int i = 0; i < index; i++) {
		// allow only one object
		if (j["ports"][i]["address"] == get_path(pdfu->dev)) {
			index = i;
			break;
		}
	}

    j["ports"][index]["address"] = get_path(pdfu->dev);
    j["ports"][index]["label"] = pdfu->alt_name;
    j["ports"][index]["protocol"] = "dfu";
    j["ports"][index]["protocolLabel"] =
        pdfu->flags & DFU_IFF_DFU ? "DFU" : "Runtime";
    j["ports"][index]["properties"] = {{"vid", print_hex(pdfu->vendor)},
                                       {"pid", print_hex(pdfu->product)},
                                       {"serialNumber", pdfu->serial_name},
                                       {"name", pdfu->alt_name}};
    index++;
  }
  return j;
}

struct dfu_if *dfu_root = NULL;
char *match_path = NULL;
int match_vendor = -1;
int match_product = -1;
int match_vendor_dfu = -1;
int match_product_dfu = -1;
int match_config_index = -1;
int match_iface_index = -1;
int match_iface_alt_index = -1;
const char *match_iface_alt_name = NULL;
const char *match_serial = NULL;
const char *match_serial_dfu = NULL;

bool getline_async(std::istream &is, std::string &str, char delim = '\n') {

  static std::string lineSoFar;
  char inChar;
  int charsRead = 0;
  bool lineRead = false;
  str = "";

  do {
    charsRead = is.readsome(&inChar, 255);
    if (charsRead == 1) {
      // if the delimiter is read then return the string so far
      if (inChar == delim) {
        str = lineSoFar;
        lineSoFar = "";
        lineRead = true;
      } else { // otherwise add it to the string so far
        lineSoFar.append(charsRead, inChar);
      }
    }
  } while (charsRead != 0 && !lineRead);

  return lineRead;
}

void print_list() {
  auto j = list_dfu_interfaces();
  std::cout << j.dump(2) << std::endl;
}




json previous_list;
void print_event() {
  auto j = list_dfu_interfaces();
  auto diff = json::diff(previous_list, j);

  json diff_discovery;
  //diff_discovery["eventType"] = "remove";

  if (diff.size() > 0) {
    for (auto& new_port : j["ports"].items()) {
      bool found = false;
      for (auto& old_port : previous_list["ports"].items()) {
        if (new_port.value()["address"] == old_port.value()["address"]) {
          found = true;
          break;
        }
      }
      if (!found) {
        diff_discovery["eventType"] = "add";
        diff_discovery["port"] = new_port.value();
        std::cout << diff_discovery.dump(2) << std::endl;
      }
    }

    for (auto& new_port : previous_list["ports"].items()) {
      bool found = false;
      for (auto& old_port : j["ports"].items()) {
        if (new_port.value()["address"] == old_port.value()["address"]) {
          found = true;
          break;
        }
      }
      if (!found) {
        diff_discovery["eventType"] = "remove";
        diff_discovery["port"] = new_port.value();
        std::cout << diff_discovery.dump(2) << std::endl;
      }
    }
    //std::cout << diff.dump(2) << std::endl;
  }
  previous_list = j;
}

enum states { IDLE, START, START_SYNC, STOP, QUIT };

int main(int argc, char **argv) {
  libusb_context *ctx;
  int ret;
  int fd;

  /* make sure all prints are flushed */
  setvbuf(stdout, NULL, _IONBF, 0);

  /* Mute stderr */
  //freopen("/dev/null", "w", stderr);

  // See
  // https://arduino.github.io/arduino-cli/0.30/pluggable-discovery-specification/

  // wait for HELLO
  for (std::string line; std::getline(std::cin, line);) {
    if (line.find("HELLO") != std::string::npos) {
      break;
    }
  }

  // Set STDIN as nonblocking
  // int flags = fcntl(0, F_GETFL, 0);
  // fcntl(0, F_SETFL, flags | O_NONBLOCK);
  json j;

  j["eventType"] = "hello";
  j["protocolVersion"] = 1;
  j["message"] = "OK";

  std::cout << j.dump(2) << std::endl;

  std::atomic<int> state = { IDLE };

  while (1) {

    std::string line;
    std::getline(std::cin, line);

    if (line.find("START_SYNC") != std::string::npos) {
      if (state == IDLE) {
        ret = libusb_init(&ctx);
        j["eventType"] = "start_sync";
        if (ret != 0) {
          j["message"] = "Permission error";
          j["error"] = true;
          std::cout << j.dump(2) << std::endl;
          continue;
        }
        std::thread([&] {
          j["message"] = "OK";
          std::cout << j.dump(2) << std::endl;
          while (1) {
            if (state == START_SYNC) {
              probe_devices(ctx);
              print_event();
            }
            std::this_thread::sleep_for(std::chrono::seconds(5));
          }
        }).detach();
      }
      state = START_SYNC;
    } else if (line.find("START") != std::string::npos) {
      state = START;
      j["eventType"] = "start";
      ret = libusb_init(&ctx);
      if (ret == 0) {
        j["message"] = "OK";
      } else {
        j["error"] = true;
        j["message"] = "Permission error";
      }
      std::cout << j.dump(2) << std::endl;
    } else if (line.find("STOP") != std::string::npos) {
      j["eventType"] = "stop";
      j["message"] = "OK";
      std::cout << j.dump(2) << std::endl;
      if (state != START_SYNC) {
        libusb_exit(ctx);
      }
      state = STOP;
    } else if (line.find("QUIT") != std::string::npos) {
      state = QUIT;
      j["eventType"] = "quit";
      j["message"] = "OK";
      std::cout << j.dump(2) << std::endl;
      exit(0);
    } else if (line.find("LIST") != std::string::npos) {
      probe_devices(ctx);
      print_list();
    }
  }
}