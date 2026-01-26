# Arduino demo review task list

This task list captures follow-up work needed to address gaps identified in the
Arduino/XIAO litterbox demo review.

## Tasks

1. **Document serial disconnect/reconnect handling**
   - Add guidance on how to restart the serial ingestion pipeline when the USB
     device drops, and clarify expected behavior on EOF/reconnect.
2. **Add an event-ingest smoke-test walkthrough**
   - Provide a minimal, repeatable sequence that shows: (a) the device emits
     contract-compliant JSON, (b) `grove_vision2_ingest` writes to the DB, and
     (c) conformance alarms appear for invalid payloads.
3. **Add cryptographic log verification steps for the Arduino demo DB**
   - Include explicit `log_verify` steps tailored to the litterbox demo path,
     with expected outcomes and troubleshooting notes.
4. **Clarify break-glass unlock and export retrieval flow for the demo**
   - Link to the break-glass docs with a short, Arduino-specific checklist for
     creating an unlock request and verifying the resulting receipts.
5. **Document database storage expectations for the demo**
   - Note the default DB path, how to override it, and where to look for receipts
     after ingestion.
6. **Add firmware configuration prerequisites**
   - Document required Wi‑Fi/NTP setup, expected AT response format, and the
     minimal changes needed if the Grove Vision AI V2 firmware output differs.
