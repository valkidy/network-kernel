using System;
using UnityEngine;

namespace NetworkExample.Kernel.Presentation
{
    [DisallowMultipleComponent]
    [RequireComponent(typeof(KernelSkeletonBinding))]
    public sealed class KernelSkeletonPoseApplicator : MonoBehaviour
    {
        private KernelSkeletonBinding binding;
        private bool bindingValidated;
        private string bindingError;

        public bool TryApply(
            KernelSkeletonRenderState state,
            SkeletonRenderStateBuffer buffer,
            out string error)
        {
            if (buffer == null)
            {
                error = "Skeleton render-state buffer is null.";
                return false;
            }

            if (binding == null)
            {
                binding = GetComponent<KernelSkeletonBinding>();
                bindingValidated = binding != null && binding.TryValidate(out bindingError);
            }
            if (!bindingValidated)
            {
                error = binding == null
                    ? "KernelSkeletonBinding is missing."
                    : bindingError;
                return false;
            }
            if (state.skeleton_asset_id != binding.SkeletonAssetId)
            {
                error =
                    $"Skeleton asset mismatch: state={state.skeleton_asset_id}, binding={binding.SkeletonAssetId}.";
                return false;
            }
            if (state.skeleton_content_hash != binding.SkeletonContentHash)
            {
                error =
                    $"Skeleton content hash mismatch: state=0x{state.skeleton_content_hash:x16}, binding=0x{binding.SkeletonContentHash:x16}.";
                return false;
            }
            if (state.bone_count != (uint)binding.Bones.Length)
            {
                error =
                    $"Bone count mismatch: state={state.bone_count}, binding={binding.Bones.Length}.";
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

            for (int index = 0; index < transforms.Count; ++index)
            {
                KernelBoneLocalTransform local = transforms.Array[transforms.Offset + index];
                Transform bone = binding.Bones[index];
                bone.localPosition = ToVector3(local.local_position);
                bone.localRotation = Normalize(local.local_rotation);
                bone.localScale = ToVector3(local.local_scale);
            }

            error = null;
            return true;
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
    }
}
