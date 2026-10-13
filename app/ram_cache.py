"""Bounded anonymous RAM for compressed assets, not decoded video surfaces.

One caller loads at a time. mmap avoids whole-file temporary byte copies, disk
writes and tmpfs swap files. Reader leases and playlist pins prohibit eviction.
Admission includes the in-flight allocation; failed admission falls back to disk.
"""
import mmap
import threading
from collections import OrderedDict
from contextlib import contextmanager
from pathlib import Path


def available_memory():
    """Linux MemAvailable constrained by cgroup v2 headroom; unknown fails closed."""
    try:
        values = dict(line.split(':', 1) for line in Path('/proc/meminfo').read_text().splitlines())
        available = int(values['MemAvailable'].split()[0]) * 1024
        limit = Path('/sys/fs/cgroup/memory.max').read_text().strip()
        if limit != 'max':
            available = min(available, max(0, int(limit) - int(Path('/sys/fs/cgroup/memory.current').read_text())))
        return available
    except (OSError, ValueError, KeyError):
        return 0


class RamCache:
    def __init__(self, budget, reserve, available=available_memory):
        self.budget = max(0, int(budget))
        self.reserve = max(0, int(reserve))
        self.available = available
        self.lock = threading.RLock()
        self.loader = threading.Lock()
        self.entries = OrderedDict()
        self.pins = set()
        self.used = self.loading = self.loads = 0
        self.epoch = 0

    @staticmethod
    def identity(file):
        stat = file.stat()
        return (stat.st_dev, stat.st_ino, stat.st_size, stat.st_mtime_ns)

    def set_window(self, files):
        with self.lock:
            pins = {str(Path(p).resolve()) for p in files}
            if pins != self.pins:
                self.epoch += 1
                self.pins = pins
            return self.epoch

    def _discard(self, key):
        entry = self.entries.pop(key)
        self.used -= entry['size']
        entry['data'].close()

    def warm(self, file, epoch=None):
        file = Path(file).resolve()
        key = str(file)
        with self.loader:
            try:
                identity = self.identity(file)
                size = identity[2]
                with self.lock:
                    if epoch is not None and epoch != self.epoch:
                        return False
                    old = self.entries.get(key)
                    if old and old['identity'] == identity:
                        self.entries.move_to_end(key)
                        return True
                    if old:
                        if old['readers']:
                            return False
                        self._discard(key)
                    if not size or size > self.budget or self.available() < size + self.reserve:
                        return False
                    for victim in list(self.entries):
                        if self.used + size <= self.budget:
                            break
                        entry = self.entries[victim]
                        if victim not in self.pins and not entry['readers']:
                            self._discard(victim)
                    if self.used + size > self.budget:
                        return False
                    self.loading = size
                data = mmap.mmap(-1, size)
                try:
                    with file.open('rb') as source:
                        offset = 0
                        while offset < size:
                            with self.lock:
                                if ((epoch is not None and epoch != self.epoch)
                                        or self.available() < self.reserve):
                                    return False
                            chunk = source.read(min(65536, size - offset))
                            if not chunk:
                                return False
                            data[offset:offset + len(chunk)] = chunk
                            offset += len(chunk)
                    if self.identity(file) != identity:
                        return False
                    with self.lock:
                        if epoch is not None and epoch != self.epoch:
                            return False
                        self.entries[key] = dict(data=data, size=size, identity=identity, readers=0)
                        self.used += size
                        self.loads += 1
                        data = None
                    return True
                finally:
                    if data is not None:
                        data.close()
            except (OSError, ValueError, MemoryError):
                return False
            finally:
                with self.lock:
                    self.loading = 0

    @contextmanager
    def acquire(self, file):
        key = str(Path(file).resolve())
        entry = None
        with self.lock:
            candidate = self.entries.get(key)
            try:
                valid = candidate and candidate['identity'] == self.identity(Path(file))
            except OSError:
                valid = False
            if valid:
                entry = candidate
                entry['readers'] += 1
                self.entries.move_to_end(key)
            elif candidate and not candidate['readers']:
                self._discard(key)
        try:
            yield entry['data'] if entry else None
        finally:
            if entry:
                with self.lock:
                    entry['readers'] -= 1

    def stats(self):
        with self.lock:
            return dict(budget=self.budget, reserve=self.reserve, bytes=self.used,
                        loading=self.loading, entries=len(self.entries), loads=self.loads)

    def close(self):
        with self.loader, self.lock:
            self.epoch += 1
            for key in list(self.entries):
                if not self.entries[key]['readers']:
                    self._discard(key)
