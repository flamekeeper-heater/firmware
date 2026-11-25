"""The apps over Wi-Fi (PROTOCOL.md): the board's web page and one
encrypted WebSocket on ESP-IDF's HTTP server. The sign-in, the frames, the records and the board
settings are the flamekeeper component's, shared with the Bluetooth service.
"""

import logging
from pathlib import Path

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import socket
from esphome.components.flamekeeper import CONF_FLAMEKEEPER_ID, FlamekeeperHub
from esphome.const import CONF_ID, CONF_PORT
from esphome.core import CORE, EsphomeError
from esphome.helpers import write_file_if_changed

from .web_page import WebPageError, collect, header

_LOGGER = logging.getLogger(__name__)

CODEOWNERS = []
DEPENDENCIES = ["esp32", "flamekeeper", "wifi"]

flamekeeper_net_ns = cg.esphome_ns.namespace("flamekeeper_net")
FlamekeeperNet = flamekeeper_net_ns.class_("FlamekeeperNet", cg.Component)

CONF_WEB_APP = "web_app"
# The local WebSockets at once (the apps on Wi-Fi), and the HTTP server's connections: those
# WebSockets and the page's files (a browser loads up to 6 at once; the least recently used
# connection is closed when all are taken). Each connection can hold socket buffers, so a board
# short of heap sets them lower.
CONF_MAX_SESSIONS = "max_sessions"
CONF_HTTP_SOCKETS = "http_sockets"
# The relay's TLS handshake starts only with this much 8-bit heap free and this big a block: the
# handshake's peak plus a margin, measured per board.
CONF_RELAY_MIN_FREE = "relay_min_free"
CONF_RELAY_MIN_BLOCK = "relay_min_block"
WEB_HEADER = "flamekeeper_web_files.h"

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(FlamekeeperNet),
            cv.GenerateID(CONF_FLAMEKEEPER_ID): cv.use_id(FlamekeeperHub),
            cv.Optional(CONF_PORT, default=80): cv.port,
            # A browser app's build folder (index.html and its assets), relative to this YAML file.
            # Left out: the board serves a placeholder page. Set but missing: the build stops.
            cv.Optional(CONF_WEB_APP): cv.string_strict,
            cv.Optional(CONF_MAX_SESSIONS, default=4): cv.int_range(min=1, max=8),
            cv.Optional(CONF_HTTP_SOCKETS, default=8): cv.int_range(min=3, max=16),
            cv.Optional(CONF_RELAY_MIN_FREE, default=32768): cv.int_range(min=8192, max=131072),
            cv.Optional(CONF_RELAY_MIN_BLOCK, default=8192): cv.int_range(min=2048, max=65536),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    lambda config: _limits(config),
    socket.consume_sockets(1, "flamekeeper_net", socket.SocketType.TCP_LISTEN),
    socket.consume_sockets(1, "flamekeeper_net", socket.SocketType.UDP),  # the server's control socket
)


def _limits(config):
    if config[CONF_HTTP_SOCKETS] < config[CONF_MAX_SESSIONS] + 2:
        raise cv.Invalid(f"{CONF_HTTP_SOCKETS} must leave 2 for the page beside the {CONF_MAX_SESSIONS}")
    return socket.consume_sockets(config[CONF_HTTP_SOCKETS] + 1, "flamekeeper_net")(config)  # + the relay


def _web_page(config) -> str:
    """The generated header's text: the app's files, or an empty table."""
    if CONF_WEB_APP not in config:
        return header([], "none (web_app not set)")
    dist = Path(CORE.relative_config_path(config[CONF_WEB_APP])).resolve()
    if not dist.is_dir():
        # Set but not built: fail, so an image never ships without the page it was meant to carry.
        raise EsphomeError(
            f"flamekeeper_net web_app: no web app at {dist}; build it first, or leave web_app out "
            "for a placeholder page"
        )
    try:
        files = collect(dist)
    except WebPageError as e:
        raise EsphomeError(f"flamekeeper_net web_app: {e}") from e
    gz = sum(len(f["gz"]) for f in files)
    _LOGGER.info("flamekeeper_net: web app from %s, %d files, %.1f KB gzipped", dist, len(files), gz / 1024)
    return header(files, str(dist))


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_hub(await cg.get_variable(config[CONF_FLAMEKEEPER_ID])))
    cg.add(var.set_port(config[CONF_PORT]))
    cg.add(var.set_limits(config[CONF_MAX_SESSIONS], config[CONF_HTTP_SOCKETS]))
    cg.add(var.set_relay_gate(config[CONF_RELAY_MIN_FREE], config[CONF_RELAY_MIN_BLOCK]))
    # In src/, beside main.cpp: ESPHome prunes files it did not copy from src/esphome/.
    write_file_if_changed(CORE.relative_src_path(WEB_HEADER), _web_page(config))
    from esphome.components.esp32 import add_idf_sdkconfig_option, include_builtin_idf_component

    include_builtin_idf_component("esp_http_server")
    add_idf_sdkconfig_option("CONFIG_HTTPD_WS_SUPPORT", True)
    # The Host / Origin checks before the handshake is answered, and the connection announced
    # after it (ESP-IDF 5 no longer calls the URI handler for the handshake).
    add_idf_sdkconfig_option("CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT", True)
    add_idf_sdkconfig_option("CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT", True)

    # The internet relay (PROTOCOL.md): an outbound WebSocket over TLS, the server
    # checked against the roots in relay_roots.pem (Google Trust Services, Let's Encrypt, SSL.com,
    # GlobalSign, DigiCert: the authorities behind Cloudflare's edge certificates and most other
    # public sites). ESP-IDF's bundle verifier looks a root up only when a server names it, so the
    # roots cost flash, not heap.
    from esphome.components.esp32 import (
        request_tls,
        require_certificate_bundle,
        require_mbedtls_sha512,
    )

    request_tls()
    require_certificate_bundle()
    require_mbedtls_sha512()  # GTS Root R4 signs with ECDSA P-384 / SHA-384
    roots = Path(__file__).parent / "relay_roots.pem"
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL", False)
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_CMN", False)
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_NONE", True)
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_CUSTOM_CERTIFICATE_BUNDLE", True)
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_CUSTOM_CERTIFICATE_BUNDLE_PATH", str(roots))
    # TLS buffers only while a record needs them, and the handshake's data freed after it: the
    # classic ESP32 runs Bluetooth, Wi-Fi and the web server beside it.
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_DYNAMIC_BUFFER", True)
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_DYNAMIC_FREE_CONFIG_DATA", True)
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_DYNAMIC_FREE_CA_CERT", True)
