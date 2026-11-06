Read-only diagnostic for the fixed M23 W202DS kernel. It maps only the existing
CH SBUF header at 0x6500d000 through the current SIPC API, reads its 68 bytes,
and examines the mapping's ARM64 page-table attribute and MAIR byte. It does
not write shared memory, send messages, reset CH, or replace any driver.
The mapping is released before module initialization returns. Build on CM2;
no automatic loading. Runtime support for sensors is not implied.
