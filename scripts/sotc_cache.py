# SPDX-License-Identifier: GPL-2.0-or-later
"""Cache location shared by releases; saves retain their selected user directory."""
from pathlib import Path
import os


def cache_path(user, env=None):
    env = os.environ if env is None else env
    if env.get('SOTC_CACHE_DIR'):
        return Path(env['SOTC_CACHE_DIR'])
    portable = env.get('SOTC_PORTABLE_CACHE', '')[:1] in ('1', 'y', 't')
    if not portable and env.get('LOCALAPPDATA'):
        return Path(env['LOCALAPPDATA']) / 'ShadowOfTheColossus-PC' / 'cache'
    return Path(user) / 'cache'
