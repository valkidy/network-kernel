using System;
using System.Runtime.InteropServices;

namespace NetworkExample.Kernel
{
    /// <summary>One line the native kernel logged.</summary>
    public readonly struct KernelLogLine
    {
        public KernelLogLine(KernelLogLevel level, ulong sequence, string text, bool truncated)
        {
            Level = level;
            Sequence = sequence;
            Text = text;
            Truncated = truncated;
        }

        public KernelLogLevel Level { get; }

        /// <summary>
        /// Counts every line captured since <see cref="KernelLog.StartCapture"/>;
        /// a gap between consecutive lines is how many were dropped.
        /// </summary>
        public ulong Sequence { get; }

        /// <summary>The line without timestamp or level.</summary>
        public string Text { get; }

        /// <summary>The native line was longer than 511 bytes and was cut.</summary>
        public bool Truncated { get; }
    }

    /// <summary>
    /// What the native kernel and game server log through spdlog. They write to
    /// stdout, which a Unity Editor never shows; this hands the same lines to
    /// managed code (Kernel_PollLogMessages).
    ///
    /// Process-wide: every kernel and game server in the library shares one
    /// logger. Capture starts with <see cref="StartCapture"/> (or the first
    /// <see cref="Poll"/>) and keeps the newest 1024 lines until drained.
    /// </summary>
    public static class KernelLog
    {
        private const int BatchSize = 64;

        private static readonly int MessageSize = Marshal.SizeOf<KernelLogMessage>();
        private static readonly int LengthOffset =
            (int)Marshal.OffsetOf<KernelLogMessage>(nameof(KernelLogMessage.length));
        private static readonly int TruncatedOffset =
            (int)Marshal.OffsetOf<KernelLogMessage>(nameof(KernelLogMessage.truncated));
        private static readonly int SequenceOffset =
            (int)Marshal.OffsetOf<KernelLogMessage>(nameof(KernelLogMessage.sequence));
        private static readonly int TextOffset =
            (int)Marshal.OffsetOf<KernelLogMessage>(nameof(KernelLogMessage.text));
        private static readonly object Gate = new object();
        private static IntPtr buffer;

        /// <summary>True when the loaded native library can capture its log.</summary>
        public static bool IsSupported
        {
            get
            {
                try
                {
                    return (KernelAbi.GetInfo().capability_flags &
                        KernelConstants.CapabilityLogCapture) != 0;
                }
                catch (Exception)
                {
                    return false;
                }
            }
        }

        /// <summary>
        /// Starts capture without reading anything. Lines logged before the
        /// first call are not captured, so call it before creating a kernel.
        /// </summary>
        public static void StartCapture()
        {
            KernelNative.Kernel_PollLogMessages(IntPtr.Zero, 0);
        }

        /// <summary>
        /// Moves up to <c>destination.Length</c> captured lines into
        /// <paramref name="destination"/>, oldest first, and returns how many.
        /// Allocates only the line strings.
        /// </summary>
        public static int Poll(KernelLogLine[] destination)
        {
            if (destination == null)
            {
                throw new ArgumentNullException(nameof(destination));
            }

            lock (Gate)
            {
                if (buffer == IntPtr.Zero)
                {
                    // Kept for the process, like the native capture it reads.
                    buffer = Marshal.AllocHGlobal(MessageSize * BatchSize);
                }

                int filled = 0;
                while (filled < destination.Length)
                {
                    int want = Math.Min(BatchSize, destination.Length - filled);
                    int count = (int)KernelNative.Kernel_PollLogMessages(buffer, (uint)want);
                    for (int index = 0; index < count; ++index)
                    {
                        destination[filled++] = Read(IntPtr.Add(buffer, index * MessageSize));
                    }

                    if (count < want)
                    {
                        break;
                    }
                }

                return filled;
            }
        }

        private static KernelLogLine Read(IntPtr message)
        {
            int length = Marshal.ReadInt32(message, LengthOffset);
            if (length < 0 || length >= KernelConstants.LogMessageTextSize)
            {
                length = 0;
            }

            return new KernelLogLine(
                (KernelLogLevel)Marshal.ReadInt32(message, 0),
                (ulong)Marshal.ReadInt64(message, SequenceOffset),
                Marshal.PtrToStringUTF8(IntPtr.Add(message, TextOffset), length),
                Marshal.ReadInt32(message, TruncatedOffset) != 0);
        }
    }
}
