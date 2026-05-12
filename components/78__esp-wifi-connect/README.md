# Hao Lab Wi-Fi Connect

Project-local override of `78/esp-wifi-connect` for the Hao Lab firmware build.

The networking behavior stays aligned with upstream:

- boot into AP + captive portal when no saved Wi-Fi works
- scan nearby access points via `/scan`
- submit credentials via `/submit`
- save newly submitted Wi-Fi credentials through `SsidManager`
- reboot through `/done.html` -> `/reboot`

The captive portal UI is intentionally customized for Hao Lab:

- single Wi-Fi provisioning flow
- no tabs, menu, language picker, or saved-network management endpoints
- no advanced settings UI or `/advanced/*` handlers
- Cream Cafe palette and Hao Lab copy

Do not use this component for protocol changes. Keep platform URLs, OTA behavior, and device authentication managed by the platform side.
