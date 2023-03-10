// This file is part of dfu-discovery.
//
// Copyright 2023 ARDUINO SA (http://www.arduino.cc/)
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

package main

/*
#cgo CPPFLAGS: -DHAVE_UNISTD_H -DHAVE_NANOSLEEP -DHAVE_ERR -I. -Idfu-util-0.11/src -I/usr/local/include/libusb-1.0 -Wall
#cgo CFLAGS: -DHAVE_UNISTD_H -DHAVE_NANOSLEEP -DHAVE_ERR -I. -Idfu-util-0.11/src -I/usr/local/include/libusb-1.0 -Wall
#cgo darwin LDFLAGS: -L/usr/local/lib -lusb-1.0 -framework IOKit -framework CoreFoundation -framework Security
#cgo !darwin LDFLAGS: -L/usr/local/lib -lusb-1.0

#include <dfu.h>
#include <dfu_util.h>
// This is missing from dfu_util.h
char *get_path(libusb_device *dev);

// Defined in main.c
const char *libusbOpen();
void libusbClose();
void dfuProbeDevices();
const char *libusbHotplugRegisterCallback();
void libusbHotplugDeregisterCallback();
int libusbHandleEvents();
*/
import "C"

import (
	"errors"
	"fmt"
	"os"

	"github.com/arduino/go-properties-orderedmap"
	discovery "github.com/arduino/pluggable-discovery-protocol-handler/v2"
)

func main() {
	dfuDisc := &DFUDiscovery{}
	disc := discovery.NewServer(dfuDisc)
	if err := disc.Run(os.Stdin, os.Stdout); err != nil {
		fmt.Fprintf(os.Stderr, "Error: %s\n", err.Error())
		os.Exit(1)
	}
}

// DFUDiscovery is the implementation of the DFU pluggable-discovery
type DFUDiscovery struct {
	close      func()
	portsCache map[string]*discovery.Port
}

// Hello is the handler for the pluggable-discovery HELLO command
func (d *DFUDiscovery) Hello(userAgent string, protocolVersion int) error {
	return nil
}

// Quit is the handler for the pluggable-discovery QUIT command
func (d *DFUDiscovery) Quit() {
	d.Stop()
}

// Stop is the handler for the pluggable-discovery STOP command
func (d *DFUDiscovery) Stop() error {
	if d.close != nil {
		d.close()
		d.close = nil
	}
	C.libusbClose()
	return nil
}

// StartSync is the handler for the pluggable-discovery START_SYNC command
func (d *DFUDiscovery) StartSync(eventCB discovery.EventCallback, errorCB discovery.ErrorCallback) error {
	d.portsCache = map[string]*discovery.Port{}
	if cErr := C.libusbOpen(); cErr != nil {
		return fmt.Errorf("can't open libusb: %s", C.GoString(cErr))
	}
	return d.sync(eventCB, errorCB)
}

func (d *DFUDiscovery) libusbSync(eventCB discovery.EventCallback, errorCB discovery.ErrorCallback) error {
	if err := C.libusbHotplugRegisterCallback(); err != nil {
		return errors.New(C.GoString(err))
	}

	closeChan := make(chan struct{})
	go func() {
		d.sendUpdates(eventCB, errorCB)
		for {
			if C.libusbHandleEvents() != 0 {
				d.sendUpdates(eventCB, errorCB)
			}
			select {
			case <-closeChan:
				C.libusbHotplugDeregisterCallback()
				return
			default:
			}
		}
	}()
	d.close = func() {
		close(closeChan)
	}
	return nil
}

func (d *DFUDiscovery) sendUpdates(eventCB discovery.EventCallback, errorCB discovery.ErrorCallback) {
	C.dfuProbeDevices()

	newCache := map[string]*discovery.Port{}
	for _, dfuIf := range d.getDFUInterfaces() {
		newPort := dfuIf.AsDiscoveryPort()
		newCache[newPort.Address] = newPort
		if _, exist := d.portsCache[newPort.Address]; !exist {
			eventCB("add", newPort)
		}
	}

	for _, oldPort := range d.portsCache {
		if _, exist := newCache[oldPort.Address]; !exist {
			eventCB("remove", oldPort)
		}
	}

	d.portsCache = newCache
}

func getPath(dev *C.struct_libusb_device) string {
	return C.GoString(C.get_path(dev))
}

// DFUInterface represents a DFU Interface
type DFUInterface struct {
	Path         string
	AltName      string
	VID, PID     uint16
	SerialNumber string
	Flags        uint32
}

// AsDiscoveryPort converts this DFUInterface into a discovery.Port
func (i *DFUInterface) AsDiscoveryPort() *discovery.Port {
	props := properties.NewMap()
	props.Set("vid", fmt.Sprintf("0x%04X", i.VID))
	props.Set("pid", fmt.Sprintf("0x%04X", i.PID))
	if i.SerialNumber != "" {
		props.Set("serial", i.SerialNumber)
	}
	// ProtocolLabel: pdfu.flags & DFU_IFF_DFU ? "DFU" : "Runtime"
	return &discovery.Port{
		Address:       i.Path,
		AddressLabel:  i.Path,
		Protocol:      "dfu",
		ProtocolLabel: "USB DFU",
		Properties:    props,
		HardwareID:    props.Get("serial"),
	}
}

func (d *DFUDiscovery) getDFUInterfaces() []*DFUInterface {
	res := []*DFUInterface{}
	for pdfu := C.dfu_root; pdfu != nil; pdfu = pdfu.next {
		if (pdfu.flags & C.DFU_IFF_DFU) == 0 {
			// decide what to do with DFU runtime objects
			// for the time being, ignore them
			continue
		}
		ifPath := getPath(pdfu.dev)
		// for i := 0; i < index; i++ {
		// 	// allow only one object
		// 	if j["ports"][i]["address"] == ifPath {
		// 		index = i
		// 		break
		// 	}
		// }

		dfuIf := &DFUInterface{
			Path:         ifPath,
			AltName:      C.GoString(pdfu.alt_name),
			VID:          uint16(pdfu.vendor),
			PID:          uint16(pdfu.product),
			SerialNumber: C.GoString(pdfu.serial_name),
		}
		res = append(res, dfuIf)
	}
	return res
}
