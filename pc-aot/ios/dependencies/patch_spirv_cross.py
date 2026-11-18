#!/usr/bin/env python3
"""Fix the sampler half of scalar combined-image aliases in pinned SPIRV-Cross.

KhronosGroup/MoltenVK#2768 documents this failure with D3D9 shaders.
Texture aliasing already exists upstream; preserve it and alias the sampler too.
No game shader or shader text substitution is involved.
"""
import argparse
import hashlib
from pathlib import Path

REVISION = "0a88b2d5c08708d45692b7096a0a84e7bfae366c"
SHA256 = "1434778fff9dccd9cf52a09848d0f5ebbf4b295042b36a4d9c85f368b734c34d"
PATCHED_SHA256 = "6a016ed8fa40bd364be62b8250a4c4a91766dcc959e31c71d898beb62cfb139c"
OLD = '\t\tstatement(descriptor_storage, " auto &", to_name(aliased_var.self), " = ", name);'
NEW = OLD + '''
		// iOS compatibility: combined images contain both a texture and a sampler.
		// The existing texture reinterpret_cast alone leaves aliasSmplr undefined.
		if (data_type.basetype == SPIRType::SampledImage && !is_array(data_type) &&
		    data_type.image.dim != DimBuffer &&
		    get_variable_data_type(base_var).basetype == SPIRType::SampledImage &&
		    !find_constexpr_sampler(aliased_var.self))
			statement(descriptor_storage, " auto &", to_sampler_expression(aliased_var.self),
			          " = ", to_sampler_expression(base_var.self), ";");'''


def patched(raw):
    if hashlib.sha256(raw).hexdigest() != SHA256:
        raise ValueError("SPIRV-Cross source differs from the reviewed revision")
    if raw.count(OLD.encode()) != 1:
        raise ValueError("sampler alias patch anchor changed")
    return raw.replace(OLD.encode(), NEW.encode(), 1)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path, nargs="?")
    parser.add_argument("--in-place", action="store_true", help="only an isolated dependency build checkout")
    args = parser.parse_args()
    raw = args.source.read_bytes()
    if args.in_place and args.output is None and hashlib.sha256(raw).hexdigest() == PATCHED_SHA256:
        print("Pinned sampler alias fix already present")
    else:
        result = patched(raw)
        if args.in_place and args.output is None:
            args.source.write_bytes(result)
        elif args.output and not args.in_place:
            with args.output.open("xb") as stream:
                stream.write(result)
        else:
            parser.error("select either a fresh output or --in-place")
