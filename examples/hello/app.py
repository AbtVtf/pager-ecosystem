"""Minimal PagerOS "hello world" app (≤ 20 LOC, PY-001 acceptance)."""

from pageros import App

app = App(name="hello")


@app.screen("/")
def home():
    return {
        "v": 1,
        "id": "scr_home",
        "body": [{"t": "text", "s": "Hello, PagerOS!"}],
    }


if __name__ == "__main__":
    app.run()
