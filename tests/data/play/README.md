# Play wire references

These files pin what libchiaking puts on the wire when it launches a stream. They are produced by running the library
against scripted fakes only: every account, device, session and server value in them is synthetic.

## Launch transcripts (`transcripts/*.txt`)

| File | Test | Driven through |
|---|---|---|
| `cloud_launch_legacy_cloud_stream_resolved_title.txt` | `core/play/adapters/links/CloudLaunchTranscript.MatchesLegacyCloudStreamResolvedTitle` | `play_test::CloudProvision::run` |
| `cloud_launch_cloud_stream_owned_title.txt` | `core/play/adapters/links/CloudLaunchTranscript.MatchesCloudStreamOwnedTitle` | `play_test::CloudProvision::run` |
| `cloud_launch_cloud_stream_busy_lock_and_entitlement_retry.txt` | `core/play/adapters/links/CloudLaunchTranscript.MatchesCloudStreamBusyLockAndEntitlementRetry` | `play_test::CloudProvision::run` |
| `cloud_launch_cloud_stream_cleared_title.txt` | `core/play/adapters/links/CloudLaunchTranscript.MatchesCloudStreamClearedTitle` | `play_test::CloudProvision::run` |
| `cloud_launch_cloud_stream_mismatched_cleared_title.txt` | `core/play/adapters/links/CloudLaunchTranscript.MatchesCloudStreamMismatchedClearedTitle` | `play_test::CloudProvision::run` |
| `internet_start_ps5.txt` | `core/play/adapters/links/InternetStartTranscript.MatchesTheScriptedPs5Start` | `play::InternetLinkConfiguration::link`, `start`, `pump_once` to streaming, `stop` |

Each test compares its run with the file byte for byte and prints the first differing line.

A transcript holds, in sent order:

- every HTTP request: method, URL, every header, and the length and FNV-1a 64 hash of the body;
- every WebSocket connection: URL, headers, subprotocol;
- every UDP socket opened and closed, and every datagram sent: destination, kind, length and FNV-1a 64 hash.

No value that stands for a secret is written. `Authorization` keeps its scheme and the length of the credential.
A header outside the plain list in `tests/unit/core/play/launch_transcript.hpp` keeps only its length. The fake
credentials, device id, session ids, session keys and auth codes the scripts hand out are replaced wherever they
appear in a URL or header by `<label len=N>`; a run fails if one of them reaches the transcript.

Nothing is normalised for being random: the scripts inject a fixed clock and fixed random bytes, so every run is
identical.

To regenerate after an intended wire change, and only then:

```sh
LIBCHIAKING_REGENERATE_GOLDEN=1 ctest --test-dir build -R 'Transcript\.Matches'
```

Review the diff of the regenerated files and commit them with the change that caused it.
