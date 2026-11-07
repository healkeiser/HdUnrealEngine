"""Write the user guide's mist volumes, samples/docs/mist_flat.vdb and samples/docs/mist_noise.vdb: a 4 x 1 x 3 m slab
of fog over the light study's floor, once uniform and once broken up by noise, as an artist builds it in Houdini
(Volume, Volume Noise Fog, Convert VDB). Run with hython:

    hython scripts/build_docs_volumes.py
"""

from pathlib import Path

import hou

OUT = Path(__file__).resolve().parents[1] / "samples" / "docs"


def slab(network):
    volume = network.createNode("volume")
    volume.parm("name").set("density")
    volume.parm("initialval1").set(
        3
    )  # extinction per metre: a 1 m slab of thick mist
    volume.parmTuple("size").set((4, 1, 3))
    volume.parmTuple("t").set((0, 0.5, 0))
    volume.parm("uniformsamples").set("max")
    volume.parm("samplediv").set(96)  # 4 cm voxels
    return volume


def noise(source):
    """Multiply the density by cloudy noise (sparse convolution): gaps, wisps and denser pockets."""
    node = source.createOutputNode("volumenoisefog")
    node.parm("operation").set("mult")
    node.parm("basis").set("sparse")
    node.parm("elementsize").set(0.6)  # the pockets' size in metres
    node.parm("oct").set(4)
    node.parm("rangemethod").set("minmax")
    node.parm("minvalue").set(
        -1.2
    )  # below 0 clamps to clear air: holes in the mist
    node.parm("maxvalue").set(1.8)
    node.parm("doclampmin").set(1)
    node.parm("clampminvalue").set(0)
    return node


def write(node, name):
    vdb = node.createOutputNode("convertvdb")
    vdb.parm("conversion").set("vdb")
    vdb.parm("vdbclass").set("fog")
    path = OUT / name
    vdb.geometry().saveToFile(str(path))
    print(f"{path}: {path.stat().st_size // 1024} KB")


def main():
    network = hou.node("/obj").createNode("geo")
    flat = slab(network)
    write(flat, "mist_flat.vdb")
    write(noise(flat), "mist_noise.vdb")


if __name__ == "__main__":
    main()
