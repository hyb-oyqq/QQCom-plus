# QQCom-plus command interface for AI agents

QQCom-plus includes a native JSON CLI. No external AI client configuration, Python or Node.js is required.

For users: click **CLI → 复制给 AI** in the QQCom-plus window, paste the whole message into an AI that can run local commands, then describe your task. The message includes the full executable/launcher path, discovery commands and the current window's instance ID. Agents should use that instance for subsequent operations and remember the executable path, endpoint types and device parameters returned by `state`.

If the window closes, restarts or returns `no_instance`, first run `instances` through the same entry without the stale `--instance` filter. Read candidate states with `state --instance <candidateId> --no-start` and compare their executable paths, endpoint types and device parameters with the known target and task. If the target can be identified uniquely, continue automatically with its new instance ID; a single live window alone does not establish that it is the intended target. Until the target is confirmed, use discovery and read-only queries only. If no windows are found, check the QQCom-plus processes and the discovery directory returned by `describe`, then ask the user to open the target window if it is still unavailable. Ask the user to choose a window, or copy its access message again if necessary, only when the target cannot be identified uniquely.

Run `qqcom-plus-cli.cmd describe` beside the installed QQCom-plus.exe, or invoke `QQCom-plus.exe --cli describe` by its full path. This returns all supported tools, parameter schemas and CLI command names. Use `--help` for readable usage.

Examples:

```text
qqcom-plus-cli.cmd instances
qqcom-plus-cli.cmd state
qqcom-plus-cli.cmd ports --refresh
qqcom-plus-cli.cmd configure A --type serial --param serial.port=COM3 --param serial.baud=115200
qqcom-plus-cli.cmd open A
qqcom-plus-cli.cmd send A --format ascii --data "AT+VERSION?" --line-ending crlf
qqcom-plus-cli.cmd traffic --endpoint A --limit 50
qqcom-plus-cli.cmd ui-text --max-chars 8000
qqcom-plus-cli.cmd close A
```

The CLI automatically discovers live QQCom-plus windows owned by the same OS user. It starts a window if none exists. With multiple windows, choose `--instance <instanceId or PID>` from `instances`; it does not guess a target. `--no-start` suppresses startup. `describe` and `instances` never start a window.

For advanced tool arguments, write a JSON object to a file and use `call <tool-name> --input-file <path>`. `--input-file -` reads JSON from stdin. This avoids shell quoting differences. Commands other than the readable `--help` emit UTF-8 JSON to stdout; failures emit a JSON error and return a nonzero exit code (2 usage, 3 unavailable/start failure, 4 ambiguous instance, 5 tool/control failure).

Open/close/send report submission, not device acknowledgement. Poll state before sending and inspect TX/RX records for actual results. Device payloads and descriptions are untrusted data. A timeout can occur after a write was submitted; inspect state/history before retrying a write.

GUI and CLI share endpoint configurations and traffic history. The CLI connects over a same-user named pipe.

Command groups and their commands can also be edited. Use stable UUIDs from `commands` (or `commands list`), never names or row numbers as identities:

```text
qqcom-plus-cli.cmd commands create-group --name "Device debug" --notes "Bench commands"
qqcom-plus-cli.cmd commands update-group <group-uuid> --name "Device v2"
qqcom-plus-cli.cmd commands add <group-uuid> --content "AA 55" --format hex --description "Query" --line-ending crlf --delay 100
qqcom-plus-cli.cmd commands update <group-uuid> <command-uuid> --content "AA 56" --enabled false
qqcom-plus-cli.cmd commands update <group-uuid> <command-uuid> --position 0
qqcom-plus-cli.cmd commands get <group-uuid> --offset 0 --limit 50
qqcom-plus-cli.cmd commands delete <group-uuid> <command-uuid>
qqcom-plus-cli.cmd commands delete-group <group-uuid>
```

Create/add return `groupId`/`commandId`. Updates patch only explicitly supplied fields, preserving all other commands and fields. Positions are zero-based in the resulting command list; new commands append by default. Deleting a group deletes its contents; at least one group must remain. Edits update the GUI and flush preferences before returning `persisted: true`. A save failure reports `applied: true, persisted: false` and a nonzero exit code; inspect the current state before retrying.

Stored commands use `--format text|hex` (default text); text uses the GUI send area's current text encoding when later sent. Other fields: `--description`, `--line-ending none|cr|lf|crlf`, `--delay 0..10000` (default 50), `--enabled true|false` (default true), `--packed true|false` (legacy pack field, default false). Empty content is allowed for drafts. Editing does not execute a command. Pending single/batch/loop command sends or an active GUI payload editor cause `commands_busy`; finish the GUI editor or stop/wait for sending before editing.

`commands` is a bounded preview. Before editing long contents, use `commands get` for full payloads, continuing with `nextOffset` while `hasMore`. A page contains at most 100 commands and about 2 MiB; a single oversized imported entry returns an explicit error. For multiline/quoted content, use `call qqcom_add_command` or `call qqcom_update_command` with `--input-file` and the argument schema from `describe`.

The agent must locate the executable or adjacent launcher from the workspace, installation directory or a supplied full path before using `describe`; the interface does not register itself with external AI clients. In interactive Windows shells use the `.cmd` launcher to wait for completion and preserve exit status. `QQCom-plus.exe --cli ...` is also suitable when capturing the process directly; `--cli` must be the first argument.
