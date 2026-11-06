# Security Policy

## Reporting

Report vulnerabilities through GitHub's private vulnerability reporting on this repository
("Security" tab, "Report a vulnerability"). Do not open public issues for them.

## Threat model (summary)

- Imported files are untrusted input: parsers are bounded, nothing in them is executed, and
  text inside them never becomes an instruction to the engine.
- The local HTTP API (planned, not built yet) will bind to 127.0.0.1 only; remote binding
  will require explicit configuration.
- The application needs no root privileges and starts no privileged services.
- Deleting a memory removes it from the database, its indexes and caches. It does **not**
  guarantee physical erasure from SSDs (wear levelling, TRIM timing), filesystem snapshots
  or backups you made.
