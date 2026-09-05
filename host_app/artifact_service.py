"""Bounded single-range parsing and manual artifact streaming."""

from dataclasses import dataclass
from pathlib import Path
from typing import Iterator, Optional


class MalformedRangeError(ValueError):
    """The Range header is malformed or unsupported by R2."""


class UnsatisfiableRangeError(ValueError):
    """The Range header is valid in shape but cannot select file bytes."""


@dataclass(frozen=True)
class ByteRange:
    start: int
    end: int

    @property
    def length(self) -> int:
        return self.end - self.start + 1


def _parse_bounded_uint(value: str, max_digits: int) -> int:
    if not value or len(value) > max_digits or not value.isascii() or not value.isdigit():
        raise MalformedRangeError("range bounds must be bounded ASCII decimal integers")
    return int(value, 10)


def parse_single_range(
    header_value: str,
    file_size: int,
    max_header_length: int = 128,
    max_number_digits: int = 20,
) -> ByteRange:
    if len(header_value) > max_header_length:
        raise MalformedRangeError("Range header is too long")
    if "," in header_value:
        raise MalformedRangeError("multiple ranges are not supported")
    if not header_value.startswith("bytes="):
        raise MalformedRangeError("only the bytes range unit is supported")

    expression = header_value[6:]
    if expression.count("-") != 1 or any(character.isspace() for character in expression):
        raise MalformedRangeError("invalid byte-range syntax")

    start_text, end_text = expression.split("-", 1)
    if not start_text and not end_text:
        raise MalformedRangeError("range bounds are missing")
    if file_size <= 0:
        raise UnsatisfiableRangeError("empty artifacts have no satisfiable byte range")

    if not start_text:
        suffix_length = _parse_bounded_uint(end_text, max_number_digits)
        if suffix_length == 0:
            raise UnsatisfiableRangeError("zero-length suffix range is unsatisfiable")
        start = max(file_size - suffix_length, 0)
        return ByteRange(start=start, end=file_size - 1)

    start = _parse_bounded_uint(start_text, max_number_digits)
    if start >= file_size:
        raise UnsatisfiableRangeError("range starts beyond the artifact")

    if not end_text:
        return ByteRange(start=start, end=file_size - 1)

    end = _parse_bounded_uint(end_text, max_number_digits)
    if end < start:
        raise UnsatisfiableRangeError("range end precedes range start")
    return ByteRange(start=start, end=min(end, file_size - 1))


def stream_file_region(
    path: Path,
    start: int,
    length: int,
    chunk_size: int,
) -> Iterator[bytes]:
    """Yield no more than ``length`` bytes from ``path`` in bounded chunks."""

    remaining = length
    with path.open("rb") as artifact:
        artifact.seek(start)
        while remaining > 0:
            chunk = artifact.read(min(chunk_size, remaining))
            if not chunk:
                raise OSError("artifact became shorter while it was being streamed")
            remaining -= len(chunk)
            yield chunk


def select_range(
    range_header: Optional[str],
    file_size: int,
    max_header_length: int,
    max_number_digits: int,
) -> Optional[ByteRange]:
    if range_header is None:
        return None
    return parse_single_range(
        range_header,
        file_size,
        max_header_length=max_header_length,
        max_number_digits=max_number_digits,
    )

