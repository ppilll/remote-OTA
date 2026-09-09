from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]


def text(relative):
    return (ROOT / relative).read_text(encoding="utf-8")


def test_uuid_registry_and_paths_are_frozen():
    protocol = text("include/edgeguard_provisioning/protocol.h")
    bluez = text("include/edgeguard_provisioning/bluez.h")
    expected = {
        "8a1d4e59-84e0-56d3-8d97-2080e5e77791",
        "6a174d82-0b81-5fa2-bca8-1cef78011280",
        "ed5101e3-0964-5afa-b52c-c653c2c1e3ca",
        "9f6e55ea-003a-5254-95dd-5806ad2fb97d",
        "6b26b3c6-d089-5b53-9fea-5e5524ce169c",
        "413bd486-a58e-5947-b570-9cffba5eec5c",
    }
    assert expected <= set(re.findall(r'"([0-9a-f-]{36})"', protocol))
    assert '"/com/edgeguard/provisioning"' in bluez
    for index in range(5):
        assert f'"/char{index}"' in bluez


def test_security_flags_and_advertisement_are_narrow():
    source = text("src/bluez.c")
    assert '"encrypt-read"' in source
    assert '"encrypt-write", "authorize"' in source
    assert "encrypt-authenticated-write" not in source
    assert '"EdgeGuard Setup"' not in source  # sourced from the frozen header
    assert "ServiceUUIDs" in source and "LocalName" in source
    for forbidden in ("Passphrase", "ssid", "attempt_id", "pairing material"):
        advertisement = source[source.index("properties_for"):source.index("return g_variant_builder_end", source.index("properties_for"))]
        assert forbidden not in advertisement


def test_no_forbidden_ota_or_shell_surface():
    sources = "\n".join(path.read_text(encoding="utf-8") for path in (ROOT / "src").glob("*.c"))
    forbidden = (
        "system(", "popen(", "rauc install", "rauc status mark-good",
        "rauc status mark-bad", "/dev/mmcblk", "/dev/disk/by-partlabel/misc",
        "factory_reset", "write-slot", "mark-active",
    )
    for token in forbidden:
        assert token not in sources
    assert "execl(\"/usr/bin/edgeguard-rk-abctl\"" in sources
    assert '"get-current"' in sources


def test_thread1_implementations_are_not_duplicated():
    owned = {path.name for path in (ROOT / "src").glob("*.c")}
    assert {"store.c", "endpoint.c", "connman.c"} <= owned
    assert len(list((ROOT / "src").glob("store*.c"))) == 1
    assert len(list((ROOT / "src").glob("endpoint*.c"))) == 1
    assert len(list((ROOT / "src").glob("connman*.c"))) == 1


def test_bounded_protocol_constants():
    protocol = text("include/edgeguard_provisioning/protocol.h")
    required = (
        "EGP_FRAME_HEADER_BYTES 32u",
        "EGP_PROTOCOL_MAX_JSON 1024u",
        "EGP_PROTOCOL_MAX_INCOMPLETE 8u",
        "EGP_PROTOCOL_IDLE_TIMEOUT_US (30 * G_USEC_PER_SEC)",
        "EGP_PROTOCOL_LIFETIME_US (60 * G_USEC_PER_SEC)",
        "EGP_PROTOCOL_REPLAY_TTL_US (10 * 60 * G_USEC_PER_SEC)",
    )
    for item in required:
        assert item in protocol
