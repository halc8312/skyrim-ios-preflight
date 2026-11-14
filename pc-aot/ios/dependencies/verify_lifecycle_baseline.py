"""Reject the failed GPU admission candidate in a production MoltenVK stage.

The original submission methods are byte-pinned; this does not claim that
background execution is safe or that the original device-loss issue is fixed.
"""
import argparse
import hashlib
from pathlib import Path

PINS = {
    'GPUObjects/MVKQueue.mm': 'd205c00ef1733d1183ea7ffe0519e8a687789dda9f66dc4f7a5183e053ca8699',
    'GPUObjects/MVKDevice.mm': '1561014e63e7a1300e1d5b4a17cdb783a8ee1e223346293421f02db5eedd9394',
    'GPUObjects/MVKImage.mm': '707d1fa4ddec9be1c9d519c3407c4fb3e166094f54fedc42a6e714c9bb609e88',
    'Commands/MVKCommandBuffer.mm': '4ddc0844ecb8b63117caec0581120d7c7bcf12a74eea196ed061f5cff559afc2',
}


def verify(root):
    for name, expected in PINS.items():
        path = Path(root) / name
        if not path.is_file() or path.is_symlink():
            raise ValueError('Original submission source required: ' + name)
        if hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            raise ValueError('Rejected or unreviewed submission source: ' + name)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    args = parser.parse_args()
    verify(args.source)
    print('Native submission baseline: 4 pinned files unchanged; rejected gate excluded')
