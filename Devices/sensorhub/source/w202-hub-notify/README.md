# CH SBUF notification diagnostic

Optional one-shot retry of `SMSG_TYPE_EVENT / SMSG_EVENT_SBUF_WRPTR` for the
already ready CHSYS destination 9, channel 4, ring 0. `notify=1` sends one bounded
message through the existing exported SIPC API. Default `notify=0` only checks
the Sensor Hub DT binding and SBUF ready state. No firmware or ring-buffer data
is written, no SIPC/channel state reset is performed, and no existing module is
unloaded or replaced. This is not a Sensor Hub driver or an autoload component.

Build only on CM2 against the unchanged matching M23 headers and the actual
SIPC `Module.symvers`. Compare TX read/write counters before and after and keep
both CH receive streams open to capture any pending query response. A successful
`smsg_send` means the local API accepted the message, not that CH consumed it.
