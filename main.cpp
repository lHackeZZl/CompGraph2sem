#include "PhongApp.h"

int WINAPI WinMain(HINSTANCE hInstance,
                   HINSTANCE /*hPrevInstance*/,
                   PSTR      /*pCmdLine*/,
                   int       /*nShowCmd*/)
{
#if defined(DEBUG) || defined(_DEBUG)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

    try
    {
        PhongApp app(hInstance);
        if (!app.Initialize()) return 0;
        return app.Run();
    }
    catch (DxException& e)
    {
        MessageBox(nullptr, e.ToString().c_str(), L"DirectX Error", MB_OK);
        return 0;
    }
}
