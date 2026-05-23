# Ask AI

Chat with Claude from your pager. A pocket assistant for quick questions on
the LILYGO T-LoRa pager (480x222).

Per-device conversation history is persisted to `askai_convos.json` next to
`app.py`. Each device sees only its own chats.

## Setup

```bash
pip install -r requirements.txt
export ANTHROPIC_API_KEY=sk-ant-...    # required for real Claude replies
```

If `ANTHROPIC_API_KEY` is unset (or the `anthropic` package is missing, or
the API call fails) the app degrades to a local echo stub so the demo still
runs end to end.

## Run

```bash
python app.py --host 127.0.0.1 --port 8017
```

Then sideload at `http://127.0.0.1:8017/`.

Optional: set `ASKAI_STORE=/path/to/store.json` to keep conversation history
somewhere other than the app directory.

## Screens

- `/` - heading "Ask AI", "New chat" button, recent conversations.
- `/new` - form to start a new chat (GET form, POST creates + replies).
- `/convo?id=X` - heading title, message log, reply form, Back / Delete.
- `/ask?id=X` (POST) - appends a turn to the chat.
- `/delete?id=X` (POST, confirm) - removes the chat.

Model: `claude-sonnet-4-6`, capped at 512 tokens, with a system prompt that
constrains replies to under 80 words so they fit the 480x222 screen.
