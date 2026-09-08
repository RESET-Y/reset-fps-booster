using System.Diagnostics;

namespace ResetFpsBooster.Core.Utilities;

/// <summary>Enables a Windows privilege on the current process token. Most privileges exist on
/// an administrator's token already but start disabled — this is the standard enable step every
/// tool that trims memory or purges the standby list (e.g. Sysinternals RAMMap) has to perform.</summary>
internal static class PrivilegeHelper
{
    public static bool EnablePrivilege(string privilegeName)
    {
        try
        {
            using var process = Process.GetCurrentProcess();
            if (!NativeMethods.OpenProcessToken(process.Handle, NativeMethods.TOKEN_ADJUST_PRIVILEGES | NativeMethods.TOKEN_QUERY, out var tokenHandle))
                return false;

            try
            {
                if (!NativeMethods.LookupPrivilegeValue(null, privilegeName, out var luid))
                    return false;

                var privileges = new NativeMethods.TOKEN_PRIVILEGES
                {
                    PrivilegeCount = 1,
                    Luid = luid,
                    Attributes = NativeMethods.SE_PRIVILEGE_ENABLED
                };

                return NativeMethods.AdjustTokenPrivileges(tokenHandle, false, ref privileges, 0, IntPtr.Zero, IntPtr.Zero);
            }
            finally
            {
                NativeMethods.CloseHandleSafe(tokenHandle);
            }
        }
        catch
        {
            return false;
        }
    }
}
