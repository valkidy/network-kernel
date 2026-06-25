import json
import pathlib
import tempfile
import unittest

from tools import kernel_rpc_codegen


class KernelRpcCodegenTest(unittest.TestCase):
    def test_generates_struct_and_method_schema(self):
        header = r'''
#define KERNEL_RPC_STRUCT(metadata)
#define KERNEL_RPC(metadata)

KERNEL_RPC_STRUCT(R"json({"type":"ExampleInput"})json")
typedef struct ExampleInput {
    uint32_t struct_size;
    uint32_t count;
    bool enabled;
} ExampleInput;

KERNEL_RPC(R"json({
  "method":"world.example",
  "authority":"developer_write",
  "phase":"simulation_tick"
})json")
bool Kernel_Example(
    KernelHandle* kernel,
    const ExampleInput* input,
    uint32_t* out_value);
'''
        model = kernel_rpc_codegen.parse_headers([("example.h", header)])

        self.assertEqual(["ExampleInput"], sorted(model.structs))
        self.assertEqual(["world.example"], [method.name for method in model.methods])
        schema = kernel_rpc_codegen.build_schema(model)
        method = schema["methods"][0]
        self.assertEqual("object", method["params"]["type"])
        self.assertEqual(
            {"input"},
            set(method["params"]["properties"]),
        )
        self.assertEqual(
            {"value"},
            set(method["result"]["properties"]),
        )
        self.assertNotIn(
            "struct_size",
            method["params"]["properties"]["input"]["properties"],
        )

    def test_rejects_fixed_arrays(self):
        header = r'''
KERNEL_RPC_STRUCT(R"json({"type":"Unsupported"})json")
typedef struct Unsupported {
    uint16_t values[4];
} Unsupported;
'''
        with self.assertRaisesRegex(
            kernel_rpc_codegen.CodegenError,
            "fixed arrays are unsupported",
        ):
            kernel_rpc_codegen.parse_headers([("unsupported.h", header)])

    def test_writes_deterministic_outputs(self):
        header = r'''
KERNEL_RPC_INTERNAL(R"json({
  "method":"dev.ping",
  "authority":"developer_read_only",
  "phase":"immediate_read_only"
})json")
bool KernelRpc_DevPing(KernelHandle* kernel);
'''
        model = kernel_rpc_codegen.parse_headers([("internal.h", header)])
        with tempfile.TemporaryDirectory() as temp_dir:
            output_dir = pathlib.Path(temp_dir)
            kernel_rpc_codegen.write_outputs(model, output_dir)
            schema = json.loads(
                (output_dir / "kernel_rpc_schema.generated.json").read_text()
            )
            self.assertEqual("dev.ping", schema["methods"][0]["method"])
            generated_header = (
                output_dir / "kernel_rpc_methods.generated.h"
            ).read_text()
            self.assertIn("generated_kernel_rpc_methods", generated_header)


if __name__ == "__main__":
    unittest.main()
