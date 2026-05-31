#!/usr/bin/env python3
"""Register a Meta Ray-Ban-like BLE advertisement through BlueZ D-Bus."""

from __future__ import annotations

import argparse
import signal
import sys

import dbus
import dbus.exceptions
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib


BLUEZ_SERVICE_NAME = "org.bluez"
LE_ADVERTISING_MANAGER_IFACE = "org.bluez.LEAdvertisingManager1"
LE_ADVERTISEMENT_IFACE = "org.bluez.LEAdvertisement1"
ADAPTER_IFACE = "org.bluez.Adapter1"
DBUS_OM_IFACE = "org.freedesktop.DBus.ObjectManager"
DBUS_PROP_IFACE = "org.freedesktop.DBus.Properties"


class InvalidArgsException(dbus.exceptions.DBusException):
    _dbus_error_name = "org.freedesktop.DBus.Error.InvalidArgs"


def parse_company_id(value: str) -> int:
    company_id = int(value, 0)
    if company_id < 0 or company_id > 0xFFFF:
        raise argparse.ArgumentTypeError("company id must fit in 16 bits")
    return company_id


class MetaRayBanAdvertisement(dbus.service.Object):
    """Advertisement object exported to BlueZ."""

    def __init__(
        self,
        bus: dbus.SystemBus,
        *,
        path: str,
        advertising_type: str,
        company_id: int,
        payload: bytes,
        local_name: str,
        include_tx_power: bool,
    ) -> None:
        self.path = path
        self.bus = bus
        self.advertising_type = advertising_type
        self.company_id = company_id
        self.payload = payload
        self.local_name = local_name
        self.include_tx_power = include_tx_power
        self.manufacturer_data = dbus.Dictionary(
            {
                dbus.UInt16(company_id): dbus.Array(list(payload), signature="y"),
            },
            signature="qv",
        )
        super().__init__(bus, self.path)

    def get_path(self) -> dbus.ObjectPath:
        return dbus.ObjectPath(self.path)

    def get_properties(self) -> dict[str, dict[str, object]]:
        properties: dict[str, object] = {
            "Type": dbus.String(self.advertising_type),
            "ManufacturerData": self.manufacturer_data,
        }

        if self.local_name:
            properties["LocalName"] = dbus.String(self.local_name)

        if self.include_tx_power:
            properties["Includes"] = dbus.Array(["tx-power"], signature="s")

        return {LE_ADVERTISEMENT_IFACE: properties}

    @dbus.service.method(DBUS_PROP_IFACE, in_signature="s", out_signature="a{sv}")
    def GetAll(self, interface: str) -> dict[str, object]:
        if interface != LE_ADVERTISEMENT_IFACE:
            raise InvalidArgsException()
        return self.get_properties()[LE_ADVERTISEMENT_IFACE]

    @dbus.service.method(LE_ADVERTISEMENT_IFACE, in_signature="", out_signature="")
    def Release(self) -> None:
        print("Advertisement released")


def find_adapter(bus: dbus.SystemBus, adapter_name: str | None) -> str | None:
    object_manager = dbus.Interface(
        bus.get_object(BLUEZ_SERVICE_NAME, "/"),
        DBUS_OM_IFACE,
    )
    objects = object_manager.GetManagedObjects()

    requested_suffix = f"/{adapter_name}" if adapter_name else None
    for path, interfaces in objects.items():
        if LE_ADVERTISING_MANAGER_IFACE not in interfaces:
            continue
        if requested_suffix and not path.endswith(requested_suffix):
            continue
        return path

    return None


def set_adapter_powered(bus: dbus.SystemBus, adapter_path: str, powered: bool) -> None:
    adapter_props = dbus.Interface(
        bus.get_object(BLUEZ_SERVICE_NAME, adapter_path),
        DBUS_PROP_IFACE,
    )
    adapter_props.Set(ADAPTER_IFACE, "Powered", dbus.Boolean(powered))


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Advertise a Meta Ray-Ban-like BLE manufacturer payload via BlueZ"
    )
    parser.add_argument(
        "--adapter",
        default="hci0",
        help="Bluetooth adapter name (default: hci0)",
    )
    parser.add_argument(
        "--type",
        choices=("peripheral", "broadcast"),
        default="peripheral",
        help="BlueZ advertisement type (default: peripheral)",
    )
    parser.add_argument(
        "--company-id",
        type=parse_company_id,
        default=0x058E,
        help="Manufacturer company id in decimal or hex (default: 0x058E)",
    )
    parser.add_argument(
        "--payload",
        default="META_RB_GLASS",
        help="ASCII manufacturer payload (default: META_RB_GLASS)",
    )
    parser.add_argument(
        "--local-name",
        default="",
        help="Advertised local name (default: none)",
    )
    tx_power_group = parser.add_mutually_exclusive_group()
    tx_power_group.add_argument(
        "--tx-power",
        dest="tx_power",
        action="store_true",
        help="Include tx-power in the advertisement",
    )
    tx_power_group.add_argument(
        "--no-tx-power",
        dest="tx_power",
        action="store_false",
        help="Do not include tx-power in the advertisement",
    )
    parser.set_defaults(tx_power=False)
    parser.add_argument(
        "--path",
        default="/org/bluez/spectacle/advertisement0",
        help="D-Bus object path for the advertisement",
    )
    args = parser.parse_args()

    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.SystemBus()

    adapter_path = find_adapter(bus, args.adapter)
    if adapter_path is None:
        print(f"No BLE advertising adapter found for {args.adapter}", file=sys.stderr)
        return 1

    set_adapter_powered(bus, adapter_path, True)

    ad_manager = dbus.Interface(
        bus.get_object(BLUEZ_SERVICE_NAME, adapter_path),
        LE_ADVERTISING_MANAGER_IFACE,
    )

    advertisement = MetaRayBanAdvertisement(
        bus,
        path=args.path,
        advertising_type=args.type,
        company_id=args.company_id,
        payload=args.payload.encode("ascii"),
        local_name=args.local_name,
        include_tx_power=args.tx_power,
    )

    main_loop = GLib.MainLoop()
    state = {"registered": False}

    def register_success() -> None:
        state["registered"] = True
        expected_hex = "FF " + f"{args.company_id & 0xFF:02X} {(args.company_id >> 8) & 0xFF:02X}"
        payload_hex = " ".join(f"{byte:02X}" for byte in args.payload.encode("ascii"))
        print(f"Using adapter: {adapter_path}")
        print(
            "Broadcasting Meta Ray-Ban test advertisement: "
            f"type={args.type} company_id=0x{args.company_id:04X} payload={args.payload!r}"
        )
        print(f"Expected manufacturer bytes: {expected_hex} {payload_hex}")
        print("Press Ctrl+C to stop.")

    def register_failure(error: Exception) -> None:
        print(f"Registration failed: {error}", file=sys.stderr)
        main_loop.quit()

    def stop(*_args: object) -> None:
        if state["registered"]:
            try:
                ad_manager.UnregisterAdvertisement(advertisement.get_path())
            except dbus.exceptions.DBusException as error:
                print(f"Unregister failed: {error}", file=sys.stderr)
            else:
                print("Stopped.")
        main_loop.quit()

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)

    ad_manager.RegisterAdvertisement(
        advertisement.get_path(),
        {},
        reply_handler=register_success,
        error_handler=register_failure,
    )

    try:
        main_loop.run()
    finally:
        advertisement.remove_from_connection()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())