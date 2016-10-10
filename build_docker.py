#!/usr/bin/env python3

import os
import shutil
import subprocess
import tempfile

ROOT = os.path.dirname(os.path.abspath(__file__))

subprocess.check_call(['bazel', 'build', 'ratelimit'], cwd=ROOT)

with tempfile.TemporaryDirectory() as workdir:
  shutil.copyfile(
    os.path.join(ROOT, '..', 'bazel-bin', 'ratelimit', 'ratelimit'),
    os.path.join(workdir, 'ratelimit')
  )
  shutil.copyfile(os.path.join(ROOT, 'Dockerfile'), os.path.join(workdir, 'Dockerfile'))

  subprocess.check_call(['docker', 'build', '.'], cwd=workdir)
