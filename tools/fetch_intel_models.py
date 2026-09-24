#!/usr/bin/env python3
"""Download Intel OMZ FP32 IR and convert to ONNX via openvino2onnx."""

from __future__ import annotations

import sys
import types
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "models"
IR = ROOT / "models" / "_ir"

MODELS = [
    "face-detection-adas-0001",
    "landmarks-regression-retail-0009",
    "face-reidentification-retail-0095",
]
BASE = "https://storage.openvinotoolkit.org/repositories/open_model_zoo/2023.0/models_bin/1"


def install_onnx_mapping_shim() -> None:
    try:
        import onnx.mapping  # noqa: F401
        return
    except Exception:
        pass
    import onnx._mapping as mapping

    sys.modules["onnx.mapping"] = mapping


def download(url: str, dest: Path) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    if dest.exists() and dest.stat().st_size > 1024:
        print(f"exists {dest} ({dest.stat().st_size})")
        return
    print(f"download {url}")
    urllib.request.urlretrieve(url, dest)
    print(f"saved {dest} ({dest.stat().st_size})")


def strip_detection_output(model):
    """ORT has no DetectionOutput; expose loc/conf/prior for CPU/GPU decode."""
    drop = {n.name for n in model.graph.node if n.op_type == "DetectionOutput"}
    drop |= {n.name for n in model.graph.node if n.op_type == "Identity" and n.name.startswith("detection_out")}
    if not drop:
        return model
    keep = [n for n in model.graph.node if n.name not in drop]
    model.graph.ClearField("node")
    model.graph.node.extend(keep)
    model.graph.ClearField("output")
    from onnx import helper, TensorProto

    model.graph.output.extend(
        [
            helper.make_tensor_value_info("mbox_loc_output7", TensorProto.FLOAT, [1, 40448]),
            helper.make_tensor_value_info("mbox_conf_flatten_output2", TensorProto.FLOAT, [1, 20224]),
            helper.make_tensor_value_info("mbox_priorbox_output7", TensorProto.FLOAT, [1, 2, 40448]),
        ]
    )
    return model


def convert_one(xml_path: Path, onnx_path: Path) -> None:
    install_onnx_mapping_shim()
    from openvino2onnx import convert
    import onnx

    print(f"convert {xml_path.name}")
    model = convert(str(xml_path), passes=[], print_passes=True, target_opset=13)
    if xml_path.name.startswith("face-detection-adas"):
        model = strip_detection_output(model)
    onnx.save_model(model, str(onnx_path))
    print(f"wrote {onnx_path} ({onnx_path.stat().st_size})")


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    for name in MODELS:
        xml = IR / name / f"{name}.xml"
        binp = IR / name / f"{name}.bin"
        download(f"{BASE}/{name}/FP32/{name}.xml", xml)
        download(f"{BASE}/{name}/FP32/{name}.bin", binp)
        convert_one(xml, OUT / f"{name}.onnx")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
