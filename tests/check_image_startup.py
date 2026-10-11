"""Exercise the real OCI entrypoint without requiring the board's GPU devices."""
import json
import os
import subprocess
import sys
import time

# Defaults suit a native host. Under QEMU user emulation (arm64 image on an
# x86_64 runner) ffprobe/ffmpeg and Python startup are many times slower, so CI
# raises these via the environment. The assertions themselves never change.
DOCKER_TIMEOUT = float(os.environ.get("SIGNAGE_STARTUP_CHECK_DOCKER_TIMEOUT", "60"))
HEALTH_TIMEOUT = float(os.environ.get("SIGNAGE_STARTUP_CHECK_HEALTH_TIMEOUT", "30"))


def docker(*args, check=True):
    return subprocess.run(
        ["docker", *args], check=check, capture_output=True, text=True,
        timeout=DOCKER_TIMEOUT,
    )


def main(image):
    # Exercise init -> shell -> Python dispatch, argv and exit-code propagation.
    result = docker(
        "run", "--rm", "--workdir", "/", "--tmpfs", "/run", "--tmpfs", "/tmp",
        image, "/usr/bin/python3", "-c",
        "import os,sys; assert os.getcwd() == '/'; "
        "assert sys.argv[1:] == ['argument with spaces']; sys.exit(37)",
        "argument with spaces", check=False,
    )
    assert result.returncode == 37, result.stdout + result.stderr

    container = docker(
        "run", "--detach", "--workdir", "/", "--tmpfs", "/run", "--tmpfs", "/tmp",
        image, "server",
    ).stdout.strip()
    try:
        deadline = time.monotonic() + HEALTH_TIMEOUT
        while True:
            probe = docker(
                "exec", container, "/usr/bin/python3", "-c",
                "from urllib.request import urlopen; "
                "assert urlopen('http://127.0.0.1:8080/healthz', timeout=1).status == 200",
                check=False,
            )
            if probe.returncode == 0:
                break
            state = json.loads(docker("inspect", container).stdout)[0]["State"]
            assert state["Running"], docker("logs", container).stdout
            if time.monotonic() >= deadline:
                raise AssertionError(
                    f"HTTP startup failed after {HEALTH_TIMEOUT:.0f}s: " + probe.stderr
                )
            time.sleep(0.2)

        docker("stop", "--time", "5", container)
        state = json.loads(docker("inspect", container).stdout)[0]["State"]
        # Python's default SIGTERM exit is 128 + 15. A forced SIGKILL is 137.
        assert state["ExitCode"] == 143, state
        assert not state["OOMKilled"], state
    finally:
        docker("rm", "--force", container)
    print("OCI entrypoint, arguments, exit status, HTTP startup and SIGTERM checks passed.")


if __name__ == "__main__":
    main(sys.argv[1])
