# External Reference Notes

## HTTP
RFC 9110 — HTTP Semantics:
`https://www.rfc-editor.org/rfc/rfc9110.html`

Relevant: Range, Accept-Ranges, Content-Range, 206 Partial Content, 416 Range Not Satisfiable.

## RAUC security
`https://rauc.readthedocs.io/en/latest/advanced.html`

RAUC bundle authenticity is based on signing/certificate/keyring verification. This is why R2 SHA256 is treated only as transfer/integrity metadata.

## RAUC bundle use / verification
`https://rauc.readthedocs.io/en/latest/using.html`

## RAUC HTTP streaming
`https://rauc.readthedocs.io/en/latest/basic.html`

RAUC documentation identifies HTTP Range support as relevant to HTTP streaming. R2 proves server single-range behavior only; it does not claim RAUC streaming was exercised.
