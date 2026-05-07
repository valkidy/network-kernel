using UnityEditor;
using UnityEngine;

namespace NetworkExample.Kernel.Editor
{
    public static class NetworkKernelHelloWorld
    {
        [MenuItem("Network Example/Kernel Hello World")]
        public static void Run()
        {
            KernelAbiInfo info = KernelAbi.GetInfo();
            Debug.Log(
                $"Hello libnetwork_kernel: abi={info.abi_version} " +
                $"capabilities=0x{info.capability_flags:x16}");
        }
    }
}
