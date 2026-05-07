"""Typed dataclasses + enums for the fxe debug protocol."""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import IntEnum
from typing import Any


class ErrorCode(IntEnum):
    PARSE_ERROR = -32700
    INVALID_REQUEST = -32600
    METHOD_NOT_FOUND = -32601
    INVALID_PARAMS = -32602
    INTERNAL = -32603
    SCRIPT_THROW = -32000
    CAPTURE_FAILED = -32001
    DETACHED = -32002
    SERVER_BUSY = -32003


@dataclass
class Handshake:
    version: str
    capabilities: list[str]
    features: dict[str, Any] = field(default_factory=dict)
    raw: dict[str, Any] = field(default_factory=dict)

    @classmethod
    def from_dict(cls, d: dict[str, Any]) -> "Handshake":
        return cls(
            version=str(d.get("version", "")),
            capabilities=list(d.get("capabilities", [])),
            features=dict(d.get("features", {})),
            raw=d,
        )


@dataclass
class EvalResult:
    value: Any = None
    type: str | None = None
    raw: dict[str, Any] = field(default_factory=dict)

    @classmethod
    def from_dict(cls, d: dict[str, Any]) -> "EvalResult":
        return cls(value=d.get("value"), type=d.get("type"), raw=d)


@dataclass
class Screenshot:
    format: str
    width: int
    height: int
    data_base64: str

    @classmethod
    def from_dict(cls, d: dict[str, Any]) -> "Screenshot":
        return cls(
            format=str(d.get("format", "png")),
            width=int(d.get("width", 0)),
            height=int(d.get("height", 0)),
            data_base64=str(d.get("dataBase64", "")),
        )


@dataclass
class MouseEvent:
    type: str
    x: float = 0.0
    y: float = 0.0
    button: str | None = None
    dx: float = 0.0
    dy: float = 0.0
    modifiers: int = 0

    def to_params(self) -> dict[str, Any]:
        p: dict[str, Any] = {"type": self.type, "x": self.x, "y": self.y}
        if self.button is not None:
            p["button"] = self.button
        if self.type == "wheel":
            p["dx"] = self.dx
            p["dy"] = self.dy
        if self.modifiers:
            p["modifiers"] = self.modifiers
        return p


@dataclass
class KeyEvent:
    type: str
    key: str
    codepoint: int | None = None
    modifiers: int = 0

    def to_params(self) -> dict[str, Any]:
        p: dict[str, Any] = {"type": self.type, "key": self.key}
        if self.codepoint is not None:
            p["codepoint"] = self.codepoint
        if self.modifiers:
            p["modifiers"] = self.modifiers
        return p


@dataclass
class ConsoleMessage:
    level: str
    text: str
    ts: float

    @classmethod
    def from_dict(cls, d: dict[str, Any]) -> "ConsoleMessage":
        return cls(
            level=str(d.get("level", "log")),
            text=str(d.get("text", "")),
            ts=float(d.get("ts", 0.0)),
        )
