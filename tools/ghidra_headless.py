"""Run Ghidra's AnalyzeHeadless directly via java.

Bypasses analyzeHeadless.bat, which breaks on paths containing parentheses.
Usage: python tools/ghidra_headless.py <AnalyzeHeadless args...>
"""
import os
import subprocess
import sys

GHIDRA = r"D:\ghidra_12.1.2_PUBLIC_20260605\ghidra_12.1.2_PUBLIC"
JAVA = r"C:\Program Files\Java\jdk-22\bin\java.exe"
MAXMEM = os.environ.get("GHIDRA_HEADLESS_MAXMEM", "12G")

VMARGS = [
    "-Djava.system.class.loader=ghidra.GhidraClassLoader",
    "-Dfile.encoding=UTF8",
    "-Duser.country=US",
    "-Duser.language=en",
    "-Duser.variant=",
    "-Dsun.java2d.opengl=false",
    "-Dsun.java2d.d3d=false",
    "-Djdk.tls.client.protocols=TLSv1.2,TLSv1.3",
    "-Dpython.console.encoding=UTF-8",
    "-Dlog4j.skipJansi=true",
    "-Xshare:off",
    "-Djavax.xml.accessExternalDTD=",
    "-Djavax.xml.accessExternalSchema=",
    "-Djavax.xml.accessExternalStylesheet=",
    "--enable-native-access=ALL-UNNAMED",
    "-Duser.home=" + os.environ["USERPROFILE"],
    "-Djava.awt.headless=true",
    "-Xmx" + MAXMEM,
]

cmd = [JAVA, *VMARGS, "-cp", GHIDRA + r"\Ghidra\Framework\Utility\lib\Utility.jar",
       "ghidra.Ghidra", "ghidra.app.util.headless.AnalyzeHeadless", *sys.argv[1:]]
sys.exit(subprocess.call(cmd))
