"""Exercise the bounded, allocation-free per-frame timing ledger."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]

class VideoLedgerTest(unittest.TestCase):
    def test_wrap_keeps_order_and_accounts_for_overwrite(self):
        source = r'''
#include "video-ledger.h"
#include <assert.h>
#include <string.h>
int main(void) {
    VideoLedger ledger = {0};
    for (uint64_t i=0; i<VIDEO_LEDGER_CAPACITY+3; i++)
        video_ledger_record(&ledger, i, i*10, i*20, 7, (uint32_t)i, 9);
    assert(ledger.total == VIDEO_LEDGER_CAPACITY+3);
    for (uint64_t i=3; i<ledger.total; i++) {
        const VideoLedgerRecord *r=video_ledger_at(&ledger, i-3);
        assert(r && r->mono_us==i && r->pts==i*10 && r->value==i*20);
        assert(r->stage==7 && r->serial==i && r->fb==9);
    }
    assert(!video_ledger_at(&ledger, VIDEO_LEDGER_CAPACITY));
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as d:
            c = pathlib.Path(d)/'test.c'; exe = pathlib.Path(d)/'test'
            c.write_text(source)
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I',str(ROOT/'native'),str(c),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)

if __name__ == '__main__': unittest.main()
