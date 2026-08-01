using System;
using NetworkExample.Kernel;
using NetworkExample.Kernel.Host;
using NetworkExample.Kernel.Presentation;
using UnityEngine;

public sealed class NetworkKernelMonsterObserverBehaviour : MonoBehaviour
{
    private const string DefaultBundleResourcePath =
        "gameplay_catalog_bundle/bundle";
    private const string DefaultEntryPath =
        "monster_observer_gameplay_catalog.yaml";
    private const uint MonsterTemplateId = 20;

    [SerializeField]
    private ushort port = 7777;

    [SerializeField]
    private TextAsset gameplayCatalogBundle;

    [SerializeField]
    private string gameplayCatalogEntryPath = DefaultEntryPath;

    [SerializeField]
    private KernelEntityPresentationWorld presentationWorld;

    [SerializeField]
    private Camera observerCamera;

    [SerializeField]
    private Vector3 cameraOffset = new Vector3(18.0f, 14.0f, -24.0f);

    [SerializeField]
    private bool createReferenceGround = true;

    private readonly RenderEntityState[] rootStates = new RenderEntityState[128];
    private readonly KernelEvent[] events = new KernelEvent[64];
    private NetworkHost host;
    private GameObject referenceGround;
    private ulong clientRenderTimeUs;
    private uint logFrame;

    private void Start()
    {
        presentationWorld = presentationWorld != null
            ? presentationWorld
            : GetComponent<KernelEntityPresentationWorld>();
        if (presentationWorld == null)
        {
            presentationWorld = gameObject.AddComponent<KernelEntityPresentationWorld>();
        }
        presentationWorld.FallbackFactory = CreatePresentationProxy;

        if (createReferenceGround)
        {
            referenceGround = CreateGround();
        }
        ResolveCamera();

        TextAsset bundle = gameplayCatalogBundle != null
            ? gameplayCatalogBundle
            : Resources.Load<TextAsset>(DefaultBundleResourcePath);
        if (bundle == null)
        {
            Fail(
                "Monster Observer could not load gameplay catalog bundle resource '" +
                DefaultBundleResourcePath + "'.");
            return;
        }

        host = new NetworkHost();
        try
        {
            bool started = host.Start(
                port,
                new GameplayCatalogServerOptions
                {
                    BundleBytes = bundle.bytes,
                    EntryPath = gameplayCatalogEntryPath,
                    ContentNamespace = "monster_observer",
                    EnableClientSync = true,
                },
                out KernelGameplayCatalogLoadResult result);
            if (!started)
            {
                Fail($"Monster Observer failed to listen on port {port}.");
                return;
            }

            Debug.Log(
                $"Monster Observer loaded catalog version={result.catalog_version} " +
                $"hash=0x{result.catalog_hash:x16} entry={gameplayCatalogEntryPath}.");
        }
        catch (Exception exception)
        {
            Fail(
                $"Monster Observer failed to load '{gameplayCatalogEntryPath}': " +
                exception.Message);
        }
    }

    private void Update()
    {
        if (host == null || !host.IsRunning)
        {
            return;
        }

        float deltaSeconds = Time.deltaTime;
        clientRenderTimeUs += SecondsToMicroseconds(deltaSeconds);
        host.Update(deltaSeconds, events);

        uint rootStateCount = presentationWorld.Present(
            host.Kernel,
            clientRenderTimeUs,
            rootStates);

        ++logFrame;
        if (logFrame % 120U == 0U)
        {
            Debug.Log(
                $"Monster Observer roots={rootStateCount} " +
                $"skeletons={presentationWorld.LastSkeletonStateCount} " +
                $"enemies={host.EnemyCount} " +
                $"local_player={host.LocalPlayerNetId}.");
        }
    }

    private void LateUpdate()
    {
        if (observerCamera == null || presentationWorld == null ||
            !presentationWorld.TryGetPresentationTransform(
                MonsterTemplateId,
                out Transform monster))
        {
            return;
        }

        Vector3 target = monster.position + Vector3.up * 2.5f;
        Vector3 desiredPosition = target + cameraOffset;
        observerCamera.transform.position = Vector3.Lerp(
            observerCamera.transform.position,
            desiredPosition,
            1.0f - Mathf.Exp(-4.0f * Time.deltaTime));
        observerCamera.transform.LookAt(target, Vector3.up);
    }

    private GameObject CreatePresentationProxy(RenderEntityState state)
    {
        if (state.template_id == MonsterTemplateId)
        {
            return MonsterSimV4Proxy.Create();
        }

        GameObject proxy = GameObject.CreatePrimitive(PrimitiveType.Capsule);
        proxy.name = $"Observer_Template_{state.template_id}";
        proxy.transform.localScale = new Vector3(1.0f, 2.0f, 1.0f);
        Collider collider = proxy.GetComponent<Collider>();
        if (collider != null)
        {
            Destroy(collider);
        }
        return proxy;
    }

    private void ResolveCamera()
    {
        if (observerCamera == null)
        {
            observerCamera = Camera.main;
        }
        if (observerCamera == null)
        {
            var cameraObject = new GameObject("Monster Observer Camera");
            observerCamera = cameraObject.AddComponent<Camera>();
        }

        observerCamera.transform.position = cameraOffset + Vector3.up * 2.5f;
        observerCamera.transform.LookAt(Vector3.up * 2.5f, Vector3.up);
    }

    private static GameObject CreateGround()
    {
        GameObject ground = GameObject.CreatePrimitive(PrimitiveType.Plane);
        ground.name = "Monster Observer Reference Ground";
        ground.transform.position = Vector3.zero;
        ground.transform.localScale = new Vector3(5.0f, 1.0f, 5.0f);
        Collider collider = ground.GetComponent<Collider>();
        if (collider != null)
        {
            Destroy(collider);
        }
        SetColor(ground, new Color(0.2f, 0.24f, 0.2f, 1.0f));
        return ground;
    }

    private void Fail(string message)
    {
        Debug.LogError(message, this);
        host?.Dispose();
        host = null;
        enabled = false;
    }

    private static ulong SecondsToMicroseconds(float seconds)
    {
        return seconds <= 0.0f ? 0UL : (ulong)(seconds * 1000000.0f);
    }

    private static void SetColor(GameObject gameObject, Color color)
    {
        Renderer renderer = gameObject.GetComponent<Renderer>();
        if (renderer == null)
        {
            return;
        }

        var properties = new MaterialPropertyBlock();
        properties.SetColor("_BaseColor", color);
        properties.SetColor("_Color", color);
        renderer.SetPropertyBlock(properties);
    }

    private void OnDestroy()
    {
        presentationWorld?.DespawnAll();
        host?.Dispose();
        host = null;
        if (referenceGround != null)
        {
            Destroy(referenceGround);
        }
    }

    private static class MonsterSimV4Proxy
    {
        private const uint SkeletonAssetId = 1;
        private const ulong SkeletonContentHash = 0x1c171165d9bb479bUL;

        private static readonly int[] ParentBoneIndices =
        {
            -1, 0, 1, 1, 1, 4, 4, 6, 6, 6,
            1, 10, 10, 12, 12, 12, 1, 16, 16, 18,
            18, 18, 1, 22, 22, 24, 24, 24, 1, 1,
            29, 29, 1, 32, 1, 34, 34, 1, 37, 1,
            0,
        };

        private static readonly ProxyBox[] Boxes =
        {
            new ProxyBox(5, new Vector3(2.20f, -2.35f, -0.30f), new Vector3(5.63f, 5.94f, 2.25f)),
            new ProxyBox(7, new Vector3(2.11f, -3.79f, 0.00f), new Vector3(4.91f, 8.93f, 0.86f)),
            new ProxyBox(8, new Vector3(1.75f, -5.45f, -0.20f), new Vector3(4.84f, 11.38f, 2.01f)),
            new ProxyBox(11, new Vector3(1.85f, -2.15f, 0.20f), new Vector3(4.96f, 5.45f, 2.04f)),
            new ProxyBox(13, new Vector3(2.45f, -4.20f, 0.30f), new Vector3(5.56f, 9.84f, 0.86f)),
            new ProxyBox(14, new Vector3(2.20f, -6.00f, 0.20f), new Vector3(5.72f, 12.53f, 2.01f)),
            new ProxyBox(17, new Vector3(-2.20f, -2.40f, -0.25f), new Vector3(5.63f, 6.00f, 2.14f)),
            new ProxyBox(19, new Vector3(-2.15f, -3.75f, -0.30f), new Vector3(4.98f, 8.88f, 0.86f)),
            new ProxyBox(20, new Vector3(-1.80f, -5.40f, -0.20f), new Vector3(4.94f, 11.30f, 2.01f)),
            new ProxyBox(23, new Vector3(-1.90f, -2.25f, 0.20f), new Vector3(5.06f, 5.64f, 2.04f)),
            new ProxyBox(25, new Vector3(-2.60f, -4.13f, 0.08f), new Vector3(5.83f, 9.78f, 1.02f)),
            new ProxyBox(26, new Vector3(-2.40f, -5.90f, 0.30f), new Vector3(6.11f, 12.40f, 2.21f)),
            new ProxyBox(28, Vector3.zero, new Vector3(5.0f, 5.0f, 4.0f)),
            new ProxyBox(30, new Vector3(-1.0f, -1.10f, -1.20f), new Vector3(2.72f, 3.01f, 3.07f)),
            new ProxyBox(33, Vector3.zero, new Vector3(3.0f, 2.2f, 3.4f)),
            new ProxyBox(36, Vector3.zero, new Vector3(3.6f, 2.8f, 3.2f)),
            new ProxyBox(38, Vector3.zero, new Vector3(2.0f, 5.0f, 1.2f)),
            new ProxyBox(39, Vector3.zero, new Vector3(9.0f, 5.0f, 6.0f)),
        };

        public static GameObject Create()
        {
            var root = new GameObject("simplified_monster_sim_v4_proxy");
            var bones = new Transform[ParentBoneIndices.Length];
            for (int index = 0; index < bones.Length; ++index)
            {
                var boneObject = new GameObject(
                    KernelSkeletonBinding.GetMonsterSimV4BoneName(index));
                Transform parent = ParentBoneIndices[index] < 0
                    ? root.transform
                    : bones[ParentBoneIndices[index]];
                boneObject.transform.SetParent(parent, false);
                bones[index] = boneObject.transform;
            }

            for (int index = 0; index < Boxes.Length; ++index)
            {
                ProxyBox definition = Boxes[index];
                GameObject box = GameObject.CreatePrimitive(PrimitiveType.Cube);
                box.name = $"Geometry_{definition.BoneIndex:D2}";
                box.transform.SetParent(bones[definition.BoneIndex], false);
                box.transform.localPosition = definition.Center;
                box.transform.localScale = definition.Size;
                Collider collider = box.GetComponent<Collider>();
                if (collider != null)
                {
                    UnityEngine.Object.Destroy(collider);
                }
                SetColor(
                    box,
                    definition.BoneIndex == 39
                        ? new Color(0.12f, 0.48f, 0.62f, 1.0f)
                        : new Color(0.2f, 0.7f, 0.78f, 1.0f));
            }

            var binding = root.AddComponent<KernelSkeletonBinding>();
            binding.SkeletonAssetId = SkeletonAssetId;
            binding.SkeletonContentHash = SkeletonContentHash;
            binding.Bones = bones;
            root.AddComponent<KernelSkeletonPoseApplicator>();
            return root;
        }

        private readonly struct ProxyBox
        {
            public ProxyBox(int boneIndex, Vector3 center, Vector3 size)
            {
                BoneIndex = boneIndex;
                Center = center;
                Size = size;
            }

            public int BoneIndex { get; }
            public Vector3 Center { get; }
            public Vector3 Size { get; }
        }
    }
}
