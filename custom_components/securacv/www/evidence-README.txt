SecuraCV evidence pack — how to check it

This zip is a signed record of what a SecuraCV witness kernel sealed: coarse
claims such as "a person or vehicle crossed the front-door zone between 14:00
and 14:10" — never faces, plates, names or footage. Everything you need to
check it is inside the zip. Open evidence_viewer.html in any current browser
(Chrome, Edge, Firefox or Safari 17 and later); it works with the network off.
Drop this whole zip onto the page, or drop the securacv-events-….json file
beside it. Nothing is uploaded and nothing is changed: the page re-derives
every signature and hash on your own computer, and checking cannot hurt the
file. Keep the zip exactly as you received it — it is the evidence; the page
is only a check of it.

Read the verdict at the top. "Verified — integrity intact: all N checks
passed" means every event is signed, chained to the one before it, and nothing
was added, removed or reordered since it was sealed. A note reading
"self-attested" means integrity is intact but authorship is not yet confirmed:
the key that signed the record traveled inside the record. To confirm who
signed it, ask the owner for their witness key fingerprint by phone or in
person (Home Assistant shows it on the SecuraCV device page; WITNESS_KEY.txt in
this zip is the owner's own copy of the same thing), compare it with the
fingerprint the page shows, then paste the key from WITNESS_KEY.txt into the
"expected device key" field and verify again — the verdict becomes
"authorship confirmed". "Not verified — rejected" means a check failed: do not
rely on this copy, ask for a fresh export, and keep both if the fresh one fails
too.

What the record does not prove: what was in the video (SecuraCV never saw the
pixels — Frigate or the camera keeps those), the exact second anything happened
(times are deliberately 10-minute windows), or who was holding the camera's
owner's keys. It proves only that these claims were sealed, in this order, in
these windows, by the key named — and that this file is exactly what was
exported.

Files in this zip:
  securacv-events-<window>.json  the sealed export, byte for byte as the kernel wrote it
  evidence_viewer.html           the offline checker (open it; it needs nothing else)
  WITNESS_KEY.txt                the owner's witness key and its fingerprint
  README.txt                     this note

Need the formal kit (RFC 3161 timestamps, the sha256sum/openssl walk-through,
an affidavit draft)? The owner can produce it with SecuraCV's court-export tool;
it checks the same bytes a different way. Project: https://github.com/kmay89/securaCV
