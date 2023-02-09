# for Mac OS 12.6.3 Apple Silicon M1 Max
# libusb-1.0 installed with homebrew
# then
# sudo ln -s /opt/homebrew/lib/libusb-1.0.dylib /usr/local/lib/libusb.dylib
g++ main.cpp -I/opt/homebrew/Cellar/libusb/1.0.26/include/libusb-1.0 -lusb -fpermissive -std=c++14 -o dfu_discoverer -v
