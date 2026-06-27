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
  "phase":"simulation_tick",
  "params":[
    {"name":"input","type":"ExampleInput","passing":"const_ptr"}
  ]
})json")
bool Kernel_Example(
    KernelHandle* kernel,
    const ExampleInput* input,
    uint32_t* out_value);

KERNEL_RPC(R"json({
  "method":"world.set_entity_health",
  "authority":"developer_write",
  "phase":"simulation_tick",
  "params":[
    {"name":"net_id","type":"uint32_t","passing":"value"},
    {"name":"hp","type":"uint16_t","passing":"value"}
  ]
})json")
bool Kernel_ServerSetEntityHealth(
    KernelHandle* kernel,
    uint32_t net_id,
    uint16_t hp);
'''
        model = kernel_rpc_codegen.parse_headers([("example.h", header)])

        self.assertEqual(["ExampleInput"], sorted(model.structs))
        self.assertEqual(
            ["world.example", "world.set_entity_health"],
            [method.name for method in model.methods],
        )
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
        health_method = schema["methods"][1]
        self.assertEqual(
            {"net_id", "hp"},
            set(health_method["params"]["properties"]),
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

    def test_rejects_reserved_rpc_namespace(self):
        header = r'''
KERNEL_RPC_INTERNAL(R"json({
  "method":"rpc.discover",
  "authority":"developer_read_only",
  "phase":"immediate_read_only"
})json")
bool KernelRpc_Discover(KernelHandle* kernel);
'''
        with self.assertRaisesRegex(
            kernel_rpc_codegen.CodegenError,
            "reserved rpc namespace",
        ):
            kernel_rpc_codegen.parse_headers([("reserved.h", header)])

    def test_rejects_public_rpc_inputs_without_annotation_params(self):
        header = r'''
KERNEL_RPC(R"json({
  "method":"world.set_entity_health",
  "authority":"developer_write",
  "phase":"simulation_tick"
})json")
bool Kernel_ServerSetEntityHealth(
    KernelHandle* kernel,
    uint32_t net_id,
    uint16_t hp);
'''
        with self.assertRaisesRegex(
            kernel_rpc_codegen.CodegenError,
            "public RPC input parameters require annotation params",
        ):
            kernel_rpc_codegen.parse_headers([("missing_params.h", header)])

    def test_validates_annotation_params_against_signature(self):
        header = r'''
KERNEL_RPC_STRUCT(R"json({"type":"KernelVec3"})json")
typedef struct KernelVec3 {
    float x;
    float y;
    float z;
} KernelVec3;

KERNEL_RPC(R"json({
  "method":"world.set_velocity",
  "authority":"developer_write",
  "phase":"simulation_tick",
  "params":[
    {"name":"net_id","type":"uint32_t","passing":"value"},
    {"name":"velocity","type":"KernelQuat","passing":"const_ptr"}
  ]
})json")
bool Kernel_ServerSetEntityVelocity(
    KernelHandle* kernel,
    uint32_t net_id,
    const KernelVec3* velocity);
'''
        with self.assertRaisesRegex(
            kernel_rpc_codegen.CodegenError,
            "annotation params do not match function signature",
        ):
            kernel_rpc_codegen.parse_headers([("mismatch.h", header)])

    def test_writes_parameter_descriptors(self):
        header = r'''
#define KERNEL_RPC_STRUCT(metadata)
#define KERNEL_RPC(metadata)

KERNEL_RPC_STRUCT(R"json({"type":"ExampleInput"})json")
typedef struct ExampleInput {
    uint32_t struct_size;
    uint32_t count;
} ExampleInput;

KERNEL_RPC(R"json({
  "method":"world.example",
  "authority":"developer_write",
  "phase":"simulation_tick",
  "params":[
    {"name":"input","type":"ExampleInput","passing":"const_ptr"}
  ]
})json")
bool Kernel_Example(
    KernelHandle* kernel,
    const ExampleInput* input,
    uint32_t* out_value);
'''
        model = kernel_rpc_codegen.parse_headers([("example.h", header)])
        with tempfile.TemporaryDirectory() as temp_dir:
            output_dir = pathlib.Path(temp_dir)
            kernel_rpc_codegen.write_outputs(model, output_dir)
            generated_header = (
                output_dir / "kernel_rpc_methods.generated.h"
            ).read_text()
            generated_source = (
                output_dir / "kernel_rpc_methods.generated.cc"
            ).read_text()

        self.assertIn("KernelRpcGeneratedParamDescriptor", generated_header)
        self.assertIn("std::span<const KernelRpcGeneratedParamDescriptor> params", generated_header)
        self.assertIn('"input"', generated_source)
        self.assertIn('"ExampleInput"', generated_source)
        self.assertIn("KernelRpcGeneratedPassing::kConstPtr", generated_source)
        self.assertIn('"value"', generated_source)
        self.assertIn("KernelRpcGeneratedDirection::kOutput", generated_source)

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
            self.assertEqual(
                "json-rpc-2.0-restricted",
                schema["protocol_profile"]["name"],
            )
            self.assertFalse(schema["protocol_profile"]["batch"])
            self.assertFalse(schema["protocol_profile"]["notifications"])
            self.assertTrue(schema["protocol_profile"]["params_may_be_omitted"])
            self.assertEqual(
                -32603,
                schema["error_codes"]["internal_error"],
            )
            self.assertEqual(
                -32004,
                schema["error_codes"]["execution_failed"],
            )
            self.assertNotIn("response_pending", schema["error_codes"])
            generated_header = (
                output_dir / "kernel_rpc_methods.generated.h"
            ).read_text()
            self.assertIn("generated_kernel_rpc_methods", generated_header)


if __name__ == "__main__":
    unittest.main()
