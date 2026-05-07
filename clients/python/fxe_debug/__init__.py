"""Puppeteer-style Python SDK for the fxe debug protocol."""

from .launcher import LaunchError, connect, launch
from .page import Key, MouseButton, Page
from .protocol import ConsoleMessage, ErrorCode, EvalResult, Handshake, Screenshot
from .client import Client, MethodNotFound, ProtocolError
from .trace import (
    layout_trace_disable,
    layout_trace_drain,
    layout_trace_enable,
    trace_drain,
    trace_install,
    trace_list,
    trace_uninstall,
)

__all__ = [
    "launch",
    "connect",
    "Page",
    "MouseButton",
    "Key",
    "Client",
    "ProtocolError",
    "MethodNotFound",
    "LaunchError",
    "ConsoleMessage",
    "Handshake",
    "EvalResult",
    "Screenshot",
    "ErrorCode",
    "trace_install",
    "trace_drain",
    "trace_uninstall",
    "trace_list",
    "layout_trace_enable",
    "layout_trace_disable",
    "layout_trace_drain",
]
