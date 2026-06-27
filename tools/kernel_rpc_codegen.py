#!/usr/bin/env python3

import argparse
import dataclasses
import json
import pathlib
import re
import sys
from typing import Dict, Iterable, List, Sequence, Tuple


class CodegenError(RuntimeError):
    pass


@dataclasses.dataclass(frozen=True)
class Field:
    c_type: str
    name: str
    pointer: str = ""
    is_const: bool = False


@dataclasses.dataclass(frozen=True)
class ParameterDescriptor:
    name: str
    c_type: str
    passing: str
    direction: str


@dataclasses.dataclass(frozen=True)
class Struct:
    name: str
    metadata: dict
    fields: Tuple[Field, ...]


@dataclasses.dataclass(frozen=True)
class Method:
    name: str
    metadata: dict
    symbol: str
    return_type: str
    parameters: Tuple[Field, ...]
    internal: bool


@dataclasses.dataclass(frozen=True)
class Model:
    structs: Dict[str, Struct]
    methods: Tuple[Method, ...]


_ANNOTATION_RE = re.compile(
    r"KERNEL_RPC_(STRUCT|INTERNAL)\s*\(\s*R\"json\((.*?)\)json\"\s*\)"
    r"|KERNEL_RPC\s*\(\s*R\"json\((.*?)\)json\"\s*\)",
    re.DOTALL,
)
_STRUCT_RE = re.compile(
    r"\s*typedef\s+struct\s+([A-Za-z_]\w*)\s*\{(.*?)\}\s*([A-Za-z_]\w*)\s*;",
    re.DOTALL,
)
_FUNCTION_RE = re.compile(
    r"\s*([A-Za-z_]\w*)\s+([A-Za-z_]\w*)\s*\((.*?)\)\s*;",
    re.DOTALL,
)
_FIELD_RE = re.compile(
    r"^\s*(const\s+)?([A-Za-z_]\w*)\s*(\*)?\s*([A-Za-z_]\w*)\s*$"
)
_SCALAR_TYPES = {
    "bool",
    "float",
    "double",
    "int8_t",
    "uint8_t",
    "int16_t",
    "uint16_t",
    "int32_t",
    "uint32_t",
    "int64_t",
    "uint64_t",
}
_INTEGER_TYPES = {
    "int8_t",
    "uint8_t",
    "int16_t",
    "uint16_t",
    "int32_t",
    "uint32_t",
    "int64_t",
    "uint64_t",
}


def _parse_metadata(text: str, source: str) -> dict:
    try:
        value = json.loads(text)
    except json.JSONDecodeError as error:
        raise CodegenError(f"{source}: invalid annotation JSON: {error}") from error
    if not isinstance(value, dict):
        raise CodegenError(f"{source}: annotation metadata must be an object")
    return value


def _split_declarations(text: str) -> Iterable[str]:
    for declaration in text.split(";"):
        declaration = re.sub(r"/\*.*?\*/", "", declaration, flags=re.DOTALL)
        declaration = re.sub(r"//.*", "", declaration).strip()
        if declaration:
            yield declaration


def _parse_field(declaration: str, source: str) -> Field:
    if "[" in declaration or "]" in declaration:
        raise CodegenError(f"{source}: fixed arrays are unsupported: {declaration}")
    match = _FIELD_RE.match(declaration)
    if match is None:
        raise CodegenError(f"{source}: unsupported declaration: {declaration}")
    return Field(
        c_type=match.group(2),
        name=match.group(4),
        pointer=match.group(3) or "",
        is_const=match.group(1) is not None,
    )


def parse_headers(headers: Sequence[Tuple[str, str]]) -> Model:
    structs: Dict[str, Struct] = {}
    methods: List[Method] = []
    method_names = set()

    for source, text in headers:
        for annotation in _ANNOTATION_RE.finditer(text):
            kind = annotation.group(1) or "METHOD"
            metadata_text = annotation.group(2) or annotation.group(3)
            metadata = _parse_metadata(metadata_text, source)
            following = text[annotation.end() :]
            if kind == "STRUCT":
                declaration = _STRUCT_RE.match(following)
                if declaration is None:
                    raise CodegenError(
                        f"{source}: KERNEL_RPC_STRUCT must precede a typedef struct"
                    )
                if declaration.group(1) != declaration.group(3):
                    raise CodegenError(
                        f"{source}: struct tag and typedef name must match"
                    )
                name = declaration.group(3)
                if name in structs:
                    raise CodegenError(f"{source}: duplicate RPC struct {name}")
                fields = tuple(
                    _parse_field(field, f"{source}:{name}")
                    for field in _split_declarations(declaration.group(2))
                )
                structs[name] = Struct(name, metadata, fields)
                continue

            declaration = _FUNCTION_RE.match(following)
            if declaration is None:
                raise CodegenError(
                    f"{source}: RPC annotation must precede a function declaration"
                )
            method_name = metadata.get("method")
            if not isinstance(method_name, str) or not method_name:
                raise CodegenError(f"{source}: RPC method metadata requires method")
            if method_name == "rpc" or method_name.startswith("rpc."):
                raise CodegenError(
                    f"{source}: reserved rpc namespace cannot be registered: "
                    f"{method_name}"
                )
            if method_name in method_names:
                raise CodegenError(f"{source}: duplicate RPC method {method_name}")
            method_names.add(method_name)
            parameters_text = declaration.group(3).strip()
            parameters = ()
            if parameters_text and parameters_text != "void":
                parameters = tuple(
                    _parse_field(parameter, f"{source}:{method_name}")
                    for parameter in parameters_text.split(",")
                )
            methods.append(
                Method(
                    name=method_name,
                    metadata=metadata,
                    symbol=declaration.group(2),
                    return_type=declaration.group(1),
                    parameters=parameters,
                    internal=kind == "INTERNAL",
                )
            )

    _validate_model(structs, methods)
    return Model(structs=structs, methods=tuple(sorted(methods, key=lambda value: value.name)))


def _validate_model(structs: Dict[str, Struct], methods: Sequence[Method]) -> None:
    for struct in structs.values():
        for field in struct.fields:
            if field.pointer:
                raise CodegenError(
                    f"{struct.name}: pointer fields are unsupported: {field.name}"
                )
            if (
                field.name != "struct_size"
                and field.c_type not in _SCALAR_TYPES
                and field.c_type not in {"KernelVec3", "KernelQuat"}
                and field.c_type not in structs
            ):
                raise CodegenError(
                    f"{struct.name}: unsupported field type {field.c_type}"
                )

    for method in methods:
        if method.return_type != "bool":
            raise CodegenError(
                f"{method.name}: only bool-returning functions are supported"
            )
        if not method.parameters:
            raise CodegenError(f"{method.name}: KernelHandle* kernel is required")
        kernel = method.parameters[0]
        if kernel.c_type != "KernelHandle" or kernel.pointer != "*" or kernel.name != "kernel":
            raise CodegenError(
                f"{method.name}: first parameter must be KernelHandle* kernel"
            )
        for parameter in method.parameters[1:]:
            if not parameter.pointer:
                if parameter.c_type not in _SCALAR_TYPES:
                    raise CodegenError(
                        f"{method.name}: unsupported scalar type {parameter.c_type}"
                    )
                continue
            if parameter.pointer != "*":
                raise CodegenError(
                    f"{method.name}: unsupported pointer parameter {parameter.name}"
                )
            if parameter.c_type in _SCALAR_TYPES and not parameter.is_const:
                continue
            if parameter.c_type not in structs:
                raise CodegenError(
                    f"{method.name}: pointer parameter {parameter.name} must use "
                    "a KERNEL_RPC_STRUCT type"
                )
            if parameter.is_const and parameter.name.startswith("out_"):
                raise CodegenError(
                    f"{method.name}: output parameter cannot be const"
                )
        _validate_annotation_params(method)


def _parameter_descriptor(parameter: Field) -> ParameterDescriptor:
    is_output = bool(parameter.pointer and not parameter.is_const)
    name = parameter.name
    if is_output and name.startswith("out_"):
        name = name[4:]
    if not parameter.pointer:
        passing = "value"
    elif parameter.is_const:
        passing = "const_ptr"
    else:
        passing = "mutable_ptr"
    return ParameterDescriptor(
        name=name,
        c_type=parameter.c_type,
        passing=passing,
        direction="output" if is_output else "input",
    )


def _method_parameter_descriptors(method: Method) -> Tuple[ParameterDescriptor, ...]:
    return tuple(
        _parameter_descriptor(parameter)
        for parameter in method.parameters[1:]
    )


def _validate_annotation_params(method: Method) -> None:
    annotation_params = method.metadata.get("params")
    expected = [
        {
            "name": descriptor.name,
            "type": descriptor.c_type,
            "passing": descriptor.passing,
        }
        for descriptor in _method_parameter_descriptors(method)
        if descriptor.direction == "input"
    ]
    if annotation_params is None and expected and not method.internal:
        raise CodegenError(
            f"{method.name}: public RPC input parameters require annotation params"
        )
    if annotation_params is None:
        return
    if not isinstance(annotation_params, list):
        raise CodegenError(f"{method.name}: annotation params must be an array")
    if annotation_params != expected:
        raise CodegenError(
            f"{method.name}: annotation params do not match function signature"
        )


def _json_type(field: Field, structs: Dict[str, Struct]) -> dict:
    if field.c_type in structs:
        return _struct_schema(structs[field.c_type], structs)
    if field.c_type in _INTEGER_TYPES:
        return {"type": "integer"}
    if field.c_type in {"float", "double"}:
        return {"type": "number"}
    if field.c_type == "bool":
        return {"type": "boolean"}
    if field.c_type == "KernelVec3":
        return {
            "type": "object",
            "properties": {
                "x": {"type": "number"},
                "y": {"type": "number"},
                "z": {"type": "number"},
            },
            "required": ["x", "y", "z"],
            "additionalProperties": False,
        }
    if field.c_type == "KernelQuat":
        return {
            "type": "object",
            "properties": {
                "x": {"type": "number"},
                "y": {"type": "number"},
                "z": {"type": "number"},
                "w": {"type": "number"},
            },
            "required": ["x", "y", "z", "w"],
            "additionalProperties": False,
        }
    raise CodegenError(f"unsupported schema type {field.c_type}")


def _struct_schema(struct: Struct, structs: Dict[str, Struct]) -> dict:
    properties = {}
    required = []
    for field in struct.fields:
        if field.name == "struct_size":
            continue
        properties[field.name] = _json_type(field, structs)
        required.append(field.name)
    return {
        "type": "object",
        "properties": properties,
        "required": required,
        "additionalProperties": False,
    }


def build_schema(model: Model) -> dict:
    methods = []
    for method in model.methods:
        params = {}
        required = []
        result = {}
        for parameter in method.parameters[1:]:
            is_output = parameter.pointer and not parameter.is_const
            name = parameter.name[4:] if parameter.name.startswith("out_") else parameter.name
            target = result if is_output else params
            target[name] = _json_type(parameter, model.structs)
            if not is_output:
                required.append(name)
        if not result:
            result["ok"] = {"type": "boolean"}
        params_schema = method.metadata.get(
            "params_schema",
            {
                "type": "object",
                "properties": params,
                "required": required,
                "additionalProperties": False,
            },
        )
        result_schema = method.metadata.get(
            "result_schema",
            {
                "type": "object",
                "properties": result,
                "required": sorted(result),
                "additionalProperties": False,
            },
        )
        methods.append(
            {
                "method": method.name,
                "authority": method.metadata["authority"],
                "phase": method.metadata["phase"],
                "implementation": method.metadata.get("implementation", "implemented"),
                "params": params_schema,
                "result": result_schema,
            }
        )
    return {
        "jsonrpc": "2.0",
        "protocol_profile": {
            "name": "json-rpc-2.0-restricted",
            "batch": False,
            "notifications": False,
            "params_style": "object",
            "params_may_be_omitted": True,
            "null_id": False,
        },
        "error_codes": {
            "parse_error": -32700,
            "invalid_request": -32600,
            "method_not_found": -32601,
            "invalid_params": -32602,
            "internal_error": -32603,
            "permission_denied": -32001,
            "wrong_execution_phase": -32002,
            "not_implemented": -32003,
            "execution_failed": -32004,
            "resource_not_found": -32005,
        },
        "methods": methods,
        "reserved_scopes": ["agent.*", "director.*", "admin.*"],
    }


def _raw_string(text: str) -> str:
    delimiter = "krpc_schema"
    return f'R"{delimiter}({text}){delimiter}"'


def _generated_passing(value: str) -> str:
    return {
        "value": "KernelRpcGeneratedPassing::kValue",
        "const_ptr": "KernelRpcGeneratedPassing::kConstPtr",
        "mutable_ptr": "KernelRpcGeneratedPassing::kMutablePtr",
    }[value]


def _generated_direction(value: str) -> str:
    return {
        "input": "KernelRpcGeneratedDirection::kInput",
        "output": "KernelRpcGeneratedDirection::kOutput",
    }[value]


def write_outputs(model: Model, output_dir: pathlib.Path) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    schema_text = json.dumps(build_schema(model), indent=2, sort_keys=True) + "\n"
    header = """#ifndef KERNEL_RPC_METHODS_GENERATED_H_
#define KERNEL_RPC_METHODS_GENERATED_H_

#include <span>
#include <string_view>

namespace network_example {

enum class KernelRpcGeneratedPassing {
    kValue,
    kConstPtr,
    kMutablePtr,
};

enum class KernelRpcGeneratedDirection {
    kInput,
    kOutput,
};

struct KernelRpcGeneratedParamDescriptor {
    const char* name;
    const char* type;
    KernelRpcGeneratedPassing passing;
    KernelRpcGeneratedDirection direction;
};

struct KernelRpcGeneratedMethodDescriptor {
    const char* method;
    const char* authority;
    const char* phase;
    const char* symbol;
    bool internal;
    std::span<const KernelRpcGeneratedParamDescriptor> params;
};

std::span<const KernelRpcGeneratedMethodDescriptor> generated_kernel_rpc_methods();
std::string_view generated_kernel_rpc_schema();

}  // namespace network_example

#endif  // KERNEL_RPC_METHODS_GENERATED_H_
"""
    param_arrays = []
    method_param_array_names = []
    for index, method in enumerate(model.methods):
        array_name = f"kMethod{index}Params"
        method_param_array_names.append(array_name)
        descriptors = _method_parameter_descriptors(method)
        param_rows = "\n".join(
            "    {"
            + ", ".join(
                [
                    json.dumps(descriptor.name),
                    json.dumps(descriptor.c_type),
                    _generated_passing(descriptor.passing),
                    _generated_direction(descriptor.direction),
                ]
            )
            + "},"
            for descriptor in descriptors
        )
        param_arrays.append(
            f"""constexpr std::array<KernelRpcGeneratedParamDescriptor, {len(descriptors)}>
    {array_name}{{{{
{param_rows}
}}}};
"""
        )
    rows = "\n".join(
        "    {"
        + ", ".join(
            [
                json.dumps(method.name),
                json.dumps(method.metadata["authority"]),
                json.dumps(method.metadata["phase"]),
                json.dumps(method.symbol),
                "true" if method.internal else "false",
                (
                    "std::span<const KernelRpcGeneratedParamDescriptor>{"
                    + method_param_array_names[index]
                    + "}"
                    if _method_parameter_descriptors(method)
                    else "std::span<const KernelRpcGeneratedParamDescriptor>{}"
                ),
            ]
        )
        + "},"
        for index, method in enumerate(model.methods)
    )
    source = f"""#include "kernel/src/kernel_rpc_methods.generated.h"

#include <array>

namespace network_example {{
namespace {{

{"".join(param_arrays)}
constexpr std::array<KernelRpcGeneratedMethodDescriptor, {len(model.methods)}>
    kMethods{{{{
{rows}
}}}};

constexpr std::string_view kSchema = {_raw_string(schema_text)};

}}  // namespace

std::span<const KernelRpcGeneratedMethodDescriptor> generated_kernel_rpc_methods() {{
    return kMethods;
}}

std::string_view generated_kernel_rpc_schema() {{
    return kSchema;
}}

}}  // namespace network_example
"""
    (output_dir / "kernel_rpc_methods.generated.h").write_text(header)
    (output_dir / "kernel_rpc_methods.generated.cc").write_text(source)
    (output_dir / "kernel_rpc_schema.generated.json").write_text(schema_text)


def main(argv: Sequence[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--header", action="append", required=True)
    parser.add_argument("--output-dir", required=True)
    args = parser.parse_args(argv)
    headers = []
    for header_path in args.header:
        path = pathlib.Path(header_path)
        headers.append((str(path), path.read_text()))
    try:
        model = parse_headers(headers)
        write_outputs(model, pathlib.Path(args.output_dir))
    except CodegenError as error:
        print(f"kernel_rpc_codegen: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
