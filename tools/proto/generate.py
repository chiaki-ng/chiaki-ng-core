import argparse
import pathlib
import subprocess
import sys
import tempfile

from google.protobuf import descriptor_pb2

ROOT = pathlib.Path(__file__).resolve().parents[2]
PROTO = ROOT / "proto"
SOURCE = PROTO / "takion.proto"
OPTIONS = PROTO / "takion.options"
GENERATED = ROOT / "src/core/net/protocol/takion/generated"


class TakionGenerator:
    def __init__(self, nanopb: pathlib.Path, work: pathlib.Path) -> None:
        self.nanopb = nanopb
        self.work = work

    def descriptor(self) -> descriptor_pb2.FileDescriptorProto:
        output = self.work / "takion.desc"
        subprocess.check_call([
            sys.executable, "-m", "grpc_tools.protoc",
            f"--proto_path={PROTO}",
            f"--descriptor_set_out={output}",
            str(SOURCE),
        ])
        files = descriptor_pb2.FileDescriptorSet.FromString(output.read_bytes())
        return files.file[0]

    def overrides(self) -> list[str]:
        retyped = {
            descriptor_pb2.FieldDescriptorProto.TYPE_STRING: "TYPE_BYTES",
            descriptor_pb2.FieldDescriptorProto.TYPE_ENUM: "TYPE_INT32",
        }
        source = self.descriptor()
        found: list[str] = []
        pending = [(source.package, message) for message in source.message_type]
        while pending:
            scope, message = pending.pop()
            name = f"{scope}.{message.name}"
            found.extend(
                f"{name}.{field.name} type_override:{retyped[field.type]}"
                for field in message.field if field.type in retyped)
            pending.extend((name, nested) for nested in message.nested_type)
        return sorted(found)

    def options(self) -> pathlib.Path:
        lines = [OPTIONS.read_text().rstrip("\n")]
        lines.extend(self.overrides())
        combined = self.work / "takion.options"
        combined.write_text("\n".join(lines) + "\n")
        return combined

    def generate(self) -> int:
        combined = self.options()
        GENERATED.mkdir(parents=True, exist_ok=True)
        return subprocess.call([
            sys.executable, str(self.nanopb / "generator" / "nanopb_generator.py"),
            "--proto-path", str(PROTO),
            "--options-file", str(combined),
            "--output-dir", str(GENERATED),
            "--strip-path",
            str(SOURCE),
        ])


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Regenerate the nanopb Takion sources from proto/takion.proto")
    parser.add_argument("--nanopb", required=True, type=pathlib.Path,
                        help="a nanopb 0.4.9.2 checkout, run with its requirements installed")
    arguments = parser.parse_args()
    with tempfile.TemporaryDirectory() as work:
        return TakionGenerator(arguments.nanopb, pathlib.Path(work)).generate()


if __name__ == "__main__":
    sys.exit(main())
