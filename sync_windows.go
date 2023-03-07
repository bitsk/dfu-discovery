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

import (
	"context"
	"time"

	"github.com/arduino/go-win32-utils/devicenotification"
	discovery "github.com/arduino/pluggable-discovery-protocol-handler/v2"
)

func (d *DFUDiscovery) sync(eventCB discovery.EventCallback, errorCB discovery.ErrorCallback) error {
	ctx, cancel := context.WithCancel(context.Background())
	d.close = cancel

	deviceEventChan := make(chan bool, 1)
	go func() {
		err := devicenotification.Start(ctx, func() {
			select {
			case deviceEventChan <- true:
			default:
			}
		}, errorCB)
		if err != nil {
			errorCB(err.Error())
		}
	}()

	go func() {
		d.sendUpdates(eventCB, errorCB)

		for {
			select {
			case <-ctx.Done():
				return
			case <-deviceEventChan:
			}

		again:
			d.sendUpdates(eventCB, errorCB)

			// Trigger another update after 500ms because Windows might signal a
			// new port much before it becomes actually available.
			select {
			case <-ctx.Done():
				return
			case <-deviceEventChan:
				goto again
			case <-time.After(time.Millisecond * 500):
			}
			d.sendUpdates(eventCB, errorCB)
		}
	}()
	return nil
}
