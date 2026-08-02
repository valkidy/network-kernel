using System;
using System.Collections.Generic;
using UnityEngine;
using UnityEngine.Serialization;

namespace NetworkExample.Kernel.Presentation
{
    [Serializable]
    public sealed class KernelEntityPresentationCatalogEntry
    {
        [FormerlySerializedAs("TemplateId")]
        [Tooltip("Actor template ID used to select this prefab.")]
        public uint ActorTemplateId;

        public GameObject Prefab;
    }

    [Serializable]
    public sealed class KernelEntityPresentationCatalog
    {
        [SerializeField]
        private KernelEntityPresentationCatalogEntry[] entries =
            Array.Empty<KernelEntityPresentationCatalogEntry>();

        public bool TryGetPrefab(RenderEntityState state, out GameObject prefab)
        {
            if (state.entity_type != KernelEntityType.Actor)
            {
                prefab = null;
                return false;
            }

            return TryGetPrefab(state.template_id, out prefab);
        }

        public bool TryGetPrefab(uint actorTemplateId, out GameObject prefab)
        {
            if (actorTemplateId == 0)
            {
                prefab = null;
                return false;
            }

            if (entries != null)
            {
                for (int index = 0; index < entries.Length; ++index)
                {
                    KernelEntityPresentationCatalogEntry entry = entries[index];
                    if (entry != null &&
                        entry.ActorTemplateId == actorTemplateId)
                    {
                        prefab = entry.Prefab;
                        return prefab != null;
                    }
                }
            }

            prefab = null;
            return false;
        }
    }

    [DisallowMultipleComponent]
    public sealed class KernelEntityPresentationWorld : MonoBehaviour
    {
        private sealed class PresentationInstance
        {
            public KernelEntityType EntityType;
            public uint TemplateId;
            public GameObject GameObject;
            public KernelSkeletonPoseApplicator SkeletonApplicator;
            public bool SkeletonErrorLogged;
        }

        [SerializeField]
        private KernelEntityPresentationCatalog catalog =
            new KernelEntityPresentationCatalog();

        [SerializeField]
        private Transform presentationRoot;

        [SerializeField]
        private bool createFallbackProxies = true;

        private readonly Dictionary<uint, PresentationInstance> instances =
            new Dictionary<uint, PresentationInstance>();
        private readonly HashSet<uint> visibleNetIds = new HashSet<uint>();
        private readonly List<uint> staleNetIds = new List<uint>();
        private readonly SkeletonRenderStateBuffer queriedSkeletonStates =
            new SkeletonRenderStateBuffer(4, 128);

        public Func<RenderEntityState, GameObject> FallbackFactory { get; set; }
        public uint LastSkeletonStateCount => queriedSkeletonStates.StateCount;

        public uint Present(
            Kernel kernel,
            ulong clientRenderTimeUs,
            RenderEntityState[] rootStates)
        {
            if (kernel == null)
            {
                throw new ArgumentNullException(nameof(kernel));
            }
            if (rootStates == null)
            {
                throw new ArgumentNullException(nameof(rootStates));
            }

            uint rootStateCount =
                kernel.GetRenderStatesAtTime(clientRenderTimeUs, rootStates);
            kernel.GetSkeletonRenderStatesAtTime(
                clientRenderTimeUs,
                queriedSkeletonStates);
            PresentInternal(
                rootStates,
                rootStateCount,
                queriedSkeletonStates,
                kernel);
            return rootStateCount;
        }

        public void Present(
            RenderEntityState[] rootStates,
            uint rootStateCount,
            SkeletonRenderStateBuffer skeletonStates)
        {
            PresentInternal(rootStates, rootStateCount, skeletonStates, null);
        }

        private void PresentInternal(
            RenderEntityState[] rootStates,
            uint rootStateCount,
            SkeletonRenderStateBuffer skeletonStates,
            Kernel kernel)
        {
            if (rootStates == null)
            {
                throw new ArgumentNullException(nameof(rootStates));
            }

            int count = rootStateCount >= (uint)rootStates.Length
                ? rootStates.Length
                : (int)rootStateCount;
            visibleNetIds.Clear();
            for (int index = 0; index < count; ++index)
            {
                RenderEntityState state = rootStates[index];
                if (state.net_id == 0)
                {
                    continue;
                }

                visibleNetIds.Add(state.net_id);
                PresentationInstance instance = ResolveInstance(state);
                if (instance == null)
                {
                    continue;
                }

                instance.GameObject.transform.SetPositionAndRotation(
                    new Vector3(state.position.x, state.position.y, state.position.z),
                    Normalize(state.rotation));
            }

            RemoveStaleInstances();
            ApplySkeletonStates(skeletonStates, kernel);
        }

        public bool TryGetPresentationTransform(uint templateId, out Transform value)
        {
            foreach (PresentationInstance instance in instances.Values)
            {
                if (instance.TemplateId == templateId && instance.GameObject != null)
                {
                    value = instance.GameObject.transform;
                    return true;
                }
            }

            value = null;
            return false;
        }

        public void DespawnAll()
        {
            foreach (PresentationInstance instance in instances.Values)
            {
                if (instance.GameObject != null)
                {
                    Destroy(instance.GameObject);
                }
            }
            instances.Clear();
            visibleNetIds.Clear();
            staleNetIds.Clear();
        }

        private PresentationInstance ResolveInstance(RenderEntityState state)
        {
            if (instances.TryGetValue(state.net_id, out PresentationInstance existing))
            {
                if (existing.EntityType == state.entity_type &&
                    existing.TemplateId == state.template_id)
                {
                    return existing;
                }

                if (existing.GameObject != null)
                {
                    Destroy(existing.GameObject);
                }
                instances.Remove(state.net_id);
            }

            GameObject instanceObject = null;
            if (catalog != null &&
                catalog.TryGetPrefab(state, out GameObject prefab))
            {
                instanceObject = Instantiate(prefab, ResolveRoot());
            }
            else if (FallbackFactory != null)
            {
                instanceObject = FallbackFactory(state);
                if (instanceObject != null)
                {
                    instanceObject.transform.SetParent(ResolveRoot(), false);
                }
            }
            else if (createFallbackProxies)
            {
                instanceObject = CreateFallbackProxy(state.template_id);
            }

            if (instanceObject == null)
            {
                return null;
            }

            instanceObject.name = $"Entity_{state.net_id}_Template_{state.template_id}";
            KernelSkeletonPoseApplicator skeletonApplicator =
                instanceObject.GetComponentInChildren<KernelSkeletonPoseApplicator>(true);
            if (skeletonApplicator == null)
            {
                KernelSkeletonBinding binding =
                    instanceObject.GetComponentInChildren<KernelSkeletonBinding>(true);
                if (binding != null)
                {
                    skeletonApplicator =
                        binding.gameObject.AddComponent<KernelSkeletonPoseApplicator>();
                }
            }

            var instance = new PresentationInstance
            {
                EntityType = state.entity_type,
                TemplateId = state.template_id,
                GameObject = instanceObject,
                SkeletonApplicator = skeletonApplicator,
            };
            instances.Add(state.net_id, instance);
            return instance;
        }

        private void ApplySkeletonStates(
            SkeletonRenderStateBuffer buffer,
            Kernel kernel)
        {
            if (buffer == null)
            {
                return;
            }

            int count = buffer.StateCount >= (uint)buffer.States.Length
                ? buffer.States.Length
                : (int)buffer.StateCount;
            for (int index = 0; index < count; ++index)
            {
                KernelSkeletonRenderState state = buffer.States[index];
                if (!instances.TryGetValue(
                        state.entity_net_id,
                        out PresentationInstance instance) ||
                    instance.SkeletonApplicator == null)
                {
                    continue;
                }

                if (!instance.SkeletonApplicator.TryStagePose(
                        kernel,
                        state,
                        buffer,
                        out string error) &&
                    !instance.SkeletonErrorLogged)
                {
                    Debug.LogError(
                        $"Failed to apply skeleton pose for entity {state.entity_net_id}: {error}",
                        instance.GameObject);
                    instance.SkeletonErrorLogged = true;
                }
            }
        }

        private void RemoveStaleInstances()
        {
            staleNetIds.Clear();
            foreach (KeyValuePair<uint, PresentationInstance> pair in instances)
            {
                if (!visibleNetIds.Contains(pair.Key))
                {
                    staleNetIds.Add(pair.Key);
                }
            }

            for (int index = 0; index < staleNetIds.Count; ++index)
            {
                uint netId = staleNetIds[index];
                PresentationInstance instance = instances[netId];
                if (instance.GameObject != null)
                {
                    Destroy(instance.GameObject);
                }
                instances.Remove(netId);
            }
        }

        private Transform ResolveRoot()
        {
            return presentationRoot != null ? presentationRoot : transform;
        }

        private GameObject CreateFallbackProxy(uint templateId)
        {
            GameObject proxy = GameObject.CreatePrimitive(PrimitiveType.Capsule);
            proxy.name = $"Template_{templateId}_Fallback";
            proxy.transform.SetParent(ResolveRoot(), false);
            Collider collider = proxy.GetComponent<Collider>();
            if (collider != null)
            {
                Destroy(collider);
            }
            return proxy;
        }

        private static Quaternion Normalize(KernelQuat value)
        {
            float lengthSquared = value.x * value.x + value.y * value.y +
                                  value.z * value.z + value.w * value.w;
            if (lengthSquared <= 1.0e-12f ||
                float.IsNaN(lengthSquared) ||
                float.IsInfinity(lengthSquared))
            {
                return Quaternion.identity;
            }

            float inverseLength = 1.0f / Mathf.Sqrt(lengthSquared);
            return new Quaternion(
                value.x * inverseLength,
                value.y * inverseLength,
                value.z * inverseLength,
                value.w * inverseLength);
        }

        private void OnDestroy()
        {
            DespawnAll();
        }
    }
}
