const fs=require('fs');
const p='C:/Users/Eto jA/FPS Booster/native/FrameBoostV2/src/main.cpp';
let s=fs.readFileSync(p,'latin1');
function rep(a,b){const n=s.split(a).length-1; if(n!==1){console.error('FAIL('+n+'): '+a.slice(0,60));process.exit(1);} s=s.replace(a,b);}

rep("            if (interpolator.GenerateFrame(device.get(), context.get(),",
    "            genStartMs = NowMs();\r\n            if (interpolator.GenerateFrame(device.get(), context.get(),");

rep("                                           DXGI_FORMAT_B8G8R8A8_UNORM, nullptr)) {",
    "                                           DXGI_FORMAT_B8G8R8A8_UNORM, nullptr)) {\r\n                genEndMs = NowMs();");

rep("                if (presenter.Present(context.get(), interpolator.GeneratedFrameTexture())) {\r\n                    const double shownAt = NowMs();",
    "                presentStartMs = NowMs();\r\n                const bool okG = presenter.Present(context.get(), interpolator.GeneratedFrameTexture());\r\n                presentReturnMs = NowMs();\r\n                if (okG) {\r\n                    const double shownAt = presentReturnMs;");

rep("        if (pairUsable && holdHalfInterval) {\r\n            WaitUntil(NowMs() + pairIntervalMs * 0.5, timer);\r\n        }",
    "        if (pairUsable && holdHalfInterval) {\r\n            const double holdFrom = NowMs();\r\n            holdRequestedMs = pairIntervalMs * 0.5;\r\n            WaitUntil(holdFrom + holdRequestedMs, timer);\r\n            holdWaitMs = NowMs() - holdFrom;\r\n        }");

rep("        if (presenter.Present(context.get(), frame.texture)) {\r\n            const double shownAt = NowMs();",
    "        const double presentStartN = NowMs();\r\n        const bool okN = presenter.Present(context.get(), frame.texture);\r\n        const double presentReturnN = NowMs();\r\n        presentStartMs = presentStartN;\r\n        presentReturnMs = presentReturnN;\r\n        if (okN) {\r\n            const double shownAt = presentReturnN;");

fs.writeFileSync(p,s,'latin1');
console.log('ok');
