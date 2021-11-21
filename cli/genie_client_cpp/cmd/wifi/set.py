from argparse import BooleanOptionalAction
from typing import List

import splatlog as logging

from genie_client_cpp.remote import Remote
from genie_client_cpp.context import Context
from genie_client_cpp.config import CONFIG


LOG = logging.getLogger(__name__)


def add_to(subparsers):
    parser = subparsers.add_parser(
        "set",
        target=run,
        help="Set WiFi configuration",
    )

    parser.add_argument(
        "-t",
        "--target",
        help=(
            "_destination_ argument for `ssh` or 'adb' to use Android debugger "
            "over a USB cable"
        ),
    )

    parser.add_argument(
        "-n",
        "--wifi-name",
        help="Network name (SSID)",
    )

    parser.add_argument(
        "-p",
        "--wifi-password",
        help="Network password (pre-shared key)",
    )

    parser.add_argument(
        "-d",
        "--dns-servers",
        action="append",
        help="DNS servers (can repeat option)",
    )

    parser.add_argument(
        "--reconfigure",
        action=BooleanOptionalAction,
        default=True,
        help="Reconfigure the interface after set",
    )


@Context.inject_current
def run(
    target: str,
    wifi_name: str,
    wifi_password: str,
    dns_servers: List[str],
    reconfigure: bool,
):
    remote = Remote.create(target)
    if wifi_name is not None and wifi_password is not None:
        remote.write_lines(
            CONFIG.xiaodu.paths.wifi_config,
            "ctrl_interface=/var/run/wpa_supplicant",
            "update_config=1",
            "network={",
            f'    ssid="{wifi_name}"',
            f'    psk="{wifi_password}"',
            "}",
        )

    # Do this _before_ DNS to try to avoid the system's prepend?
    if reconfigure:
        remote.run("wpa_cli", "reconfigure")

    if dns_servers:
        remote.write_lines(
            CONFIG.xiaodu.paths.dns_config,
            *(f"nameserver {server}" for server in dns_servers)
        )
