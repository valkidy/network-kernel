using System;
using UnityEngine;

namespace NetworkExample.Kernel.Presentation
{
    [DisallowMultipleComponent]
    [RequireComponent(typeof(KernelSkeletonBinding))]
    [DefaultExecutionOrder(10000)]
    public sealed class KernelSkeletonPoseApplicator : MonoBehaviour
    {
        private KernelSkeletonBinding binding;
        private bool bindingValidated;
        private string bindingError;
        private Vector3[] prefabBindPositions;
        private Quaternion[] prefabBindRotations;
        private Vector3[] prefabBindScales;
        private Vector3[] nativeReferencePositions;
        private Quaternion[] nativeReferenceRotations;
        private Vector3[] nativeReferenceScales;
        private bool nativeReferenceValid;
        private KernelSkeletonRenderState pendingState;
        private SkeletonRenderStateBuffer pendingBuffer;
        private bool hasPendingPose;
        private bool applyErrorLogged;

        public bool TryStagePose(
            KernelSkeletonRenderState state,
            SkeletonRenderStateBuffer buffer,
            out string error)
        {
            return TryStagePose(null, state, buffer, out error);
        }

        public bool TryStagePose(
            Kernel kernel,
            KernelSkeletonRenderState state,
            SkeletonRenderStateBuffer buffer,
            out string error)
        {
            if (!TryValidatePose(state, buffer, out error))
            {
                return false;
            }
            if (!EnsureNativeReferencePose(kernel, out error))
            {
                return false;
            }

            pendingState = state;
            pendingBuffer = buffer;
            hasPendingPose = true;
            return true;
        }

        public bool TryApply(
            KernelSkeletonRenderState state,
            SkeletonRenderStateBuffer buffer,
            out string error)
        {
            return TryApply(null, state, buffer, out error);
        }

        public bool TryApply(
            Kernel kernel,
            KernelSkeletonRenderState state,
            SkeletonRenderStateBuffer buffer,
            out string error)
        {
            if (!TryValidatePose(state, buffer, out error))
            {
                return false;
            }
            if (!EnsureNativeReferencePose(kernel, out error))
            {
                return false;
            }

            ArraySegment<KernelBoneLocalTransform> transforms =
                buffer.GetBoneTransforms(state);
            for (int index = 0; index < transforms.Count; ++index)
            {
                KernelBoneLocalTransform local =
                    transforms.Array[transforms.Offset + index];
                Transform bone = binding.Bones[index];
                Vector3 nativePosition = ToVector3(local.local_position);
                Quaternion nativeRotation = Normalize(local.local_rotation);
                Vector3 nativeScale = ToVector3(local.local_scale);
                if (binding.PreservePrefabBindPose)
                {
                    bone.localPosition = prefabBindPositions[index] +
                        nativePosition - nativeReferencePositions[index];
                    Quaternion rotationDelta =
                        Quaternion.Inverse(nativeReferenceRotations[index]) *
                        nativeRotation;
                    bone.localRotation =
                        prefabBindRotations[index] * rotationDelta;
                    bone.localScale = Vector3.Scale(
                        prefabBindScales[index],
                        DivideScale(nativeScale, nativeReferenceScales[index]));
                }
                else
                {
                    bone.localPosition = nativePosition;
                    bone.localRotation = nativeRotation;
                    bone.localScale = nativeScale;
                }
            }

            error = null;
            return true;
        }

        public bool TrySetNativeBindPose(
            KernelBoneLocalTransform[] nativeBindPose,
            out string error)
        {
            if (!EnsureBinding(out error))
            {
                return false;
            }
            if (nativeBindPose == null ||
                nativeBindPose.Length != binding.Bones.Length)
            {
                error =
                    $"Native bind-pose count must be {binding.Bones.Length}.";
                return false;
            }

            int count = nativeBindPose.Length;
            var positions = new Vector3[count];
            var rotations = new Quaternion[count];
            var scales = new Vector3[count];
            for (int index = 0; index < count; ++index)
            {
                KernelBoneLocalTransform local = nativeBindPose[index];
                if (!IsFinite(local) ||
                    QuaternionLengthSquared(local.local_rotation) <= 1.0e-12f)
                {
                    error = $"Native bind-pose transform {index} is invalid.";
                    return false;
                }
                positions[index] = ToVector3(local.local_position);
                rotations[index] = Normalize(local.local_rotation);
                scales[index] = ToVector3(local.local_scale);
            }

            nativeReferencePositions = positions;
            nativeReferenceRotations = rotations;
            nativeReferenceScales = scales;
            nativeReferenceValid = true;
            error = null;
            return true;
        }

        private bool TryValidatePose(
            KernelSkeletonRenderState state,
            SkeletonRenderStateBuffer buffer,
            out string error)
        {
            if (buffer == null)
            {
                error = "Skeleton render-state buffer is null.";
                return false;
            }
            if (!EnsureBinding(out error))
            {
                return false;
            }
            if (state.skeleton_asset_id != binding.SkeletonAssetId)
            {
                error =
                    $"Skeleton asset mismatch: state={state.skeleton_asset_id}, " +
                    $"binding={binding.SkeletonAssetId}.";
                return false;
            }
            if (state.skeleton_content_hash != binding.SkeletonContentHash)
            {
                error =
                    $"Skeleton content hash mismatch: " +
                    $"state=0x{state.skeleton_content_hash:x16}, " +
                    $"binding=0x{binding.SkeletonContentHash:x16}.";
                return false;
            }
            if (state.bone_count != (uint)binding.Bones.Length)
            {
                error =
                    $"Bone count mismatch: state={state.bone_count}, " +
                    $"binding={binding.Bones.Length}.";
                return false;
            }

            ArraySegment<KernelBoneLocalTransform> transforms;
            try
            {
                transforms = buffer.GetBoneTransforms(state);
            }
            catch (ArgumentOutOfRangeException exception)
            {
                error = exception.Message;
                return false;
            }

            for (int index = 0; index < transforms.Count; ++index)
            {
                KernelBoneLocalTransform local = transforms.Array[transforms.Offset + index];
                if (!IsFinite(local))
                {
                    error = $"Bone transform {index} contains a non-finite value.";
                    return false;
                }
                if (QuaternionLengthSquared(local.local_rotation) <= 1.0e-12f)
                {
                    error = $"Bone transform {index} contains a zero-length rotation.";
                    return false;
                }
            }

            error = null;
            return true;
        }

        private bool EnsureBinding(out string error)
        {
            if (binding == null)
            {
                binding = GetComponent<KernelSkeletonBinding>();
            }
            if (!bindingValidated)
            {
                bindingValidated =
                    binding != null && binding.TryValidate(out bindingError);
                if (bindingValidated)
                {
                    CapturePrefabBindPose();
                }
            }
            error = bindingValidated
                ? null
                : binding == null
                    ? "KernelSkeletonBinding is missing."
                    : bindingError;
            return bindingValidated;
        }

        private void CapturePrefabBindPose()
        {
            int count = binding.Bones.Length;
            prefabBindPositions = new Vector3[count];
            prefabBindRotations = new Quaternion[count];
            prefabBindScales = new Vector3[count];
            for (int index = 0; index < count; ++index)
            {
                Transform bone = binding.Bones[index];
                prefabBindPositions[index] = bone.localPosition;
                prefabBindRotations[index] = bone.localRotation;
                prefabBindScales[index] = bone.localScale;
            }
        }

        private bool EnsureNativeReferencePose(Kernel kernel, out string error)
        {
            if (!binding.PreservePrefabBindPose || nativeReferenceValid)
            {
                error = null;
                return true;
            }
            if (kernel == null)
            {
                error =
                    "Native skeleton bind pose is unavailable. Stage or apply " +
                    "the pose with a Kernel instance first.";
                return false;
            }

            int count = binding.Bones.Length;
            var nativeBindPose = new KernelBoneLocalTransform[count];
            uint requiredCount = kernel.GetSkeletonBindPose(
                binding.SkeletonAssetId,
                binding.SkeletonContentHash,
                nativeBindPose);
            if (requiredCount != (uint)count)
            {
                error =
                    $"Native bind-pose mismatch: query returned {requiredCount} " +
                    $"bones for asset={binding.SkeletonAssetId}, " +
                    $"hash=0x{binding.SkeletonContentHash:x16}; expected {count}.";
                return false;
            }
            return TrySetNativeBindPose(nativeBindPose, out error);
        }

        private void LateUpdate()
        {
            if (!hasPendingPose)
            {
                return;
            }

            hasPendingPose = false;
            if (!TryApply(pendingState, pendingBuffer, out string error) &&
                !applyErrorLogged)
            {
                Debug.LogError(
                    $"Failed to apply staged skeleton pose: {error}",
                    this);
                applyErrorLogged = true;
            }
        }

        private static bool IsFinite(KernelBoneLocalTransform transform)
        {
            return IsFinite(transform.local_position.x) &&
                   IsFinite(transform.local_position.y) &&
                   IsFinite(transform.local_position.z) &&
                   IsFinite(transform.local_rotation.x) &&
                   IsFinite(transform.local_rotation.y) &&
                   IsFinite(transform.local_rotation.z) &&
                   IsFinite(transform.local_rotation.w) &&
                   IsFinite(transform.local_scale.x) &&
                   IsFinite(transform.local_scale.y) &&
                   IsFinite(transform.local_scale.z);
        }

        private static bool IsFinite(float value)
        {
            return !float.IsNaN(value) && !float.IsInfinity(value);
        }

        private static float QuaternionLengthSquared(KernelQuat value)
        {
            return value.x * value.x + value.y * value.y +
                   value.z * value.z + value.w * value.w;
        }

        private static Quaternion Normalize(KernelQuat value)
        {
            float inverseLength = 1.0f / Mathf.Sqrt(QuaternionLengthSquared(value));
            return new Quaternion(
                value.x * inverseLength,
                value.y * inverseLength,
                value.z * inverseLength,
                value.w * inverseLength);
        }

        private static Vector3 ToVector3(KernelVec3 value)
        {
            return new Vector3(value.x, value.y, value.z);
        }

        private static Vector3 DivideScale(Vector3 value, Vector3 reference)
        {
            return new Vector3(
                DivideScaleComponent(value.x, reference.x),
                DivideScaleComponent(value.y, reference.y),
                DivideScaleComponent(value.z, reference.z));
        }

        private static float DivideScaleComponent(float value, float reference)
        {
            return Mathf.Abs(reference) > 1.0e-8f
                ? value / reference
                : 1.0f;
        }
    }
}
