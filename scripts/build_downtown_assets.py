"""Compatibility entry point for the current Japanese street asset generator."""
from pathlib import Path
import runpy

if __name__ == '__main__':
    runpy.run_path(str(Path(__file__).with_name('build_japanese_streets.py')),run_name='__main__')