# Signal / Teams bridge for Space Clock

Runs on a NAS (Docker) next to `bbernhard/signal-cli-rest-api` in `json-rpc` mode.
Core2 clocks poll `GET /api/messages`, reply with `POST /api/reply`, and mark
conversations read with `POST /api/read` (header `X-Token`). The admin page at
`/` is LAN-only: link the Signal account (QR code), set the LAN/public URLs,
manage the access token, and sign in to Microsoft Teams (device code, Graph
`Chat.ReadWrite`).

State lives in `./bridge-data` (`config.json` holds the token and the Teams
refresh token — keep it private; it is not part of this repository).
Expose the bridge publicly only through something like Cloudflare Tunnel; the
API always requires the token and the admin page refuses tunnelled requests.
