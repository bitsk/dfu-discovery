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

//go:build !windows

package main

import (
	discovery "github.com/arduino/pluggable-discovery-protocol-handler/v2"
)

func (d *DFUDiscovery) sync(eventCB discovery.EventCallback, errorCB discovery.ErrorCallback) error {
	return d.libusbSync(eventCB, errorCB)
}
