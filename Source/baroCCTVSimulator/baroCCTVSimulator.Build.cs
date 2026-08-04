// Copyright gbox3d. All Rights Reserved.

using UnrealBuildTool;

public class baroCCTVSimulator : ModuleRules
{
	public baroCCTVSimulator(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Public: 공개 헤더가 이 모듈들의 타입을 노출하므로 소비 모듈(게임 모듈)도 include 경로가 필요하다.
		//   - HTTP:       CenteringClientComponent.h 가 HttpFwd.h(FHttpRequestPtr 등)를 노출
		//   - HTTPServer: HucomsServerSubsystem.h 가 IHttpRouter.h / HttpResultCallback.h 를 노출
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core", "CoreUObject", "Engine", "InputCore",
			"HTTP", "HTTPServer"
		});

		// Private: 구현부(.cpp)에서만 쓰는 의존성.
		//   - Json:                Hucoms 응답 / baro_vla 추론 JSON 파싱
		//   - Sockets, Networking: FMjpegStreamServer 연속 MJPEG TCP 스트림
		//   - ImageCore:           FImage(PTZCaptureComponent/CenteringClientComponent 캡처) 직접 사용 — 전이의존 대신 명시
		//   - Projects:            IPluginManager — .uplugin VersionName 을 런타임에 읽어 /scene/catalog 로 노출
		//   - RHI:                 GRHISupportsRayTracing — persistent ViewState 허용 판정(PTZCaptureComponent)
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Json", "Sockets", "Networking", "ImageCore", "Projects", "RHI", "RenderCore", "ImageWrapper"
		});

		// /scene/help 가 서빙하는 계약 산문. C++ 에 박지 않고 파일로 두는 이유: 문구 수정에
		// 리빌드·버전범프 사슬이 걸리면 문서가 낡는 쪽으로 기운다. 패키징 빌드에도 원본
		// 텍스트 그대로 실린다(NonUFS — pak 밖 루즈 파일이라 배포 후에도 편집 가능).
		RuntimeDependencies.Add("$(PluginDir)/docs/scene-help.md", StagedFileType.NonUFS);
	}
}
