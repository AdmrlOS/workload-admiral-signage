import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]

class FdoImageTest(unittest.TestCase):
    def test_image_builds_pinned_backend_before_browser(self):
        docker = (ROOT / 'Dockerfile').read_text()
        self.assertIn('COPY scripts/build-wpe-fdo.sh /opt/signage/scripts/build-wpe-fdo.sh', docker)
        self.assertLess(docker.index('RUN sh /opt/signage/scripts/build-wpe-fdo.sh'), docker.index('RUN make -C /opt/signage/src/native'))
        self.assertIn('SIGNAGE_UI_MODIFIER=auto', docker)

if __name__ == '__main__':
    unittest.main()
