package dumper;

import com.seibel.distanthorizons.core.config.ConfigHandler;
import com.seibel.distanthorizons.core.dependencyInjection.SingletonInjector;
import com.seibel.distanthorizons.core.render.renderer.DebugWireframeHandler;
import com.seibel.distanthorizons.core.wrapperInterfaces.minecraft.IMinecraftRenderWrapper;
import com.seibel.distanthorizons.core.wrapperInterfaces.minecraft.IMinecraftSharedWrapper;

import java.io.File;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Comparator;
import java.util.stream.Stream;

/** Additional bindings required by specific suites (added as they are discovered). */
final class Extra
{
	private Extra() { }

	static void bindAll()
	{
		try
		{
			// Config: ConfigHandler resolves <installDir>/config/DistantHorizons.toml. Use a throw-away directory
			// that is wiped on every run so that every Config value starts at its default.
			Path install = new File(System.getProperty("golden.scratch", "tools/golden-dumper/build/scratch")).toPath().toAbsolutePath();
			if (Files.exists(install))
			{
				try (Stream<Path> w = Files.walk(install)) { w.sorted(Comparator.reverseOrder()).forEach(p -> p.toFile().delete()); }
			}
			Files.createDirectories(install);
			File installFile = install.toFile();
			SingletonInjector.INSTANCE.bind(IMinecraftSharedWrapper.class,
					Stub.of(IMinecraftSharedWrapper.class).on("getInstallationDirectory", a -> installFile).val("supportsSurfaceGeneration", true).build());
			ConfigHandler.tryRunFirstTimeSetup();
			// DhLightingEngine / DebugWireframeHandler read IMinecraftRenderWrapper + DebugWireframeHandler in static initialisers
			SingletonInjector.INSTANCE.bind(IMinecraftRenderWrapper.class, Stub.of(IMinecraftRenderWrapper.class).build());
			SingletonInjector.INSTANCE.bind(DebugWireframeHandler.class, DebugWireframeHandler.INSTANCE);
		}
		catch (Exception e) { throw new RuntimeException(e); }
	}
}
