# R2 Interview Talking Points

## Why no Agent in R2?
To isolate and verify the server/API/transport contract before introducing device update state and installation risk.

## Why Server and Agent separated?
Server publishes release state and bytes; Agent owns local state, decision logic, retries and later installation coordination.

## Why server manifest != RAUC manifest?
The JSON manifest is an EdgeGuard network release descriptor. A RAUC manifest belongs to the RAUC bundle/install format.

## Why SHA256 != RAUC signature?
SHA256 detects transfer/content mismatch against an expected digest. RAUC signatures authenticate a bundle against trusted certificates/keyrings.

## Why HTTP first?
R2 is an experimental-LAN protocol/deployment validation stage.

## Why HTTPS later?
TLS adds certificate provisioning, trust lifecycle and authorization concerns that are separate from proving the R2 server contract.

## Why Codex tests != target validation?
Codex cannot prove RK3588 `wlan0`, real network routing, or target-side HTTP/checksum behavior.

## Why no Buildroot change?
The existing target already had the HTTP/network/checksum capability required for R2.
