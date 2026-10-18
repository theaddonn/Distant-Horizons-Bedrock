package dumper;

import com.seibel.distanthorizons.api.enums.worldGeneration.EDhApiGeneratorPlan;
import com.seibel.distanthorizons.core.config.Config;
import com.seibel.distanthorizons.core.config.types.ConfigEntry;
import com.seibel.distanthorizons.core.multiplayer.config.SessionConfig;
import com.seibel.distanthorizons.core.multiplayer.fullData.FullDataPayload;
import com.seibel.distanthorizons.core.network.messages.AbstractNetworkMessage;
import com.seibel.distanthorizons.core.network.messages.AbstractTrackableMessage;
import com.seibel.distanthorizons.core.network.messages.MessageRegistry;
import com.seibel.distanthorizons.core.network.messages.base.CloseReasonMessage;
import com.seibel.distanthorizons.core.network.messages.base.CodecCrashMessage;
import com.seibel.distanthorizons.core.network.messages.base.LevelInitMessage;
import com.seibel.distanthorizons.core.network.messages.base.RequestLevelInitMessage;
import com.seibel.distanthorizons.core.network.messages.base.SessionConfigMessage;
import com.seibel.distanthorizons.core.network.messages.fullData.FullDataPartialUpdateMessage;
import com.seibel.distanthorizons.core.network.messages.fullData.FullDataSourceRequestMessage;
import com.seibel.distanthorizons.core.network.messages.fullData.FullDataSourceResponseMessage;
import com.seibel.distanthorizons.core.network.messages.fullData.FullDataSplitMessage;
import com.seibel.distanthorizons.core.network.messages.requests.CancelMessage;
import com.seibel.distanthorizons.core.network.messages.requests.ExceptionMessage;
import com.seibel.distanthorizons.core.pos.blockPos.DhBlockPos;
import com.seibel.distanthorizons.core.sql.dto.BeaconBeamDTO;
import com.seibel.distanthorizons.coreapi.ModInfo;
import io.netty.buffer.ByteBuf;
import io.netty.buffer.Unpooled;

import java.awt.Color;
import java.lang.reflect.Field;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Random;

/**
 * net: network message encodings (Netty ByteBuf, big-endian) + SessionConfig constraining.
 * Every message case: out = {"id":messageId,"body":hex,"frame":hex,"roundTrip":bool}
 *   body  = message.encode(buf)            (trackable messages start with i32 futureId = (int) futureId)
 *   frame = i16 ModInfo.PROTOCOL_VERSION (16) + u16 messageId + body   (AbstractPluginPacketSender.encodeMessage, non-forge path)
 *   roundTrip = decode(body) into a fresh instance, re-encode == body.
 * Message ids (MessageRegistry): 1 CloseReason, 2 LevelInit, 3 RequestLevelInit, 4 SessionConfig, 5 Cancel, 6 Exception,
 * 7 FullDataSourceRequest, 8 FullDataSourceResponse, 9 FullDataPartialUpdate, 10 FullDataSplit, 11 CodecCrash (dev build only).
 * Strings are encoded as u16 byte-length + UTF-8.
 * Case names: close_reason_N, level_init_N, request_level_init_N, session_config_N, cancel_N, exception_N, data_request_N,
 * data_response_N, partial_update_N, split_N, codec_crash_N, session_constrain_N.
 */
public class NetSuite implements Suite
{
	@Override public String name() { return "net"; }

	private static final int PER = 24;
	private static sun.misc.Unsafe unsafe;

	private static <T> T alloc(Class<T> c) throws Exception { return c.cast(unsafe.allocateInstance(c)); }

	private static void setField(Object o, Class<?> declaring, String name, Object v) throws Exception
	{
		Field f = declaring.getDeclaredField(name);
		f.setAccessible(true);
		f.set(o, v);
	}

	@Override
	public List<Obj> cases() throws Exception
	{
		Field uf = sun.misc.Unsafe.class.getDeclaredField("theUnsafe");
		uf.setAccessible(true);
		unsafe = (sun.misc.Unsafe) uf.get(null);

		Gen g = new Gen(0x4E37L);
		List<Obj> out = new ArrayList<>();

		for (int i = 0; i < PER; i++)
		{
			String reason = str(g);
			out.add(msg("close_reason_" + i, new Obj().p("reason", reason), new CloseReasonMessage(reason)));
		}
		for (int i = 0; i < PER; i++)
		{
			String dim = str(g), sk = str(g), lk = str(g);
			long t = g.anyLong();
			LevelInitMessage m = new LevelInitMessage(dim, sk, lk);
			m.serverTime = t;
			out.add(msg("level_init_" + i, new Obj().p("dimensionResourceLocation", dim).p("serverKey", sk).p("levelKey", lk).p("serverTime", t), m));
		}
		for (int i = 0; i < PER; i++)
		{
			String dim = str(g);
			out.add(msg("request_level_init_" + i, new Obj().p("dimensionResourceLocation", dim), new RequestLevelInitMessage(dim)));
		}
		for (int i = 0; i < PER; i++)
		{
			Object[] vals = setRandomConfig(g.r);
			SessionConfig sc = new SessionConfig();
			out.add(msg("session_config_" + i, new Obj().p("values", Arrays.asList(vals)), new SessionConfigMessage(sc)));
		}
		for (int i = 0; i < PER; i++)
		{
			long id = g.anyLong();
			CancelMessage m = alloc(CancelMessage.class);
			setField(m, AbstractTrackableMessage.class, "futureId", id);
			out.add(msg("cancel_" + i, new Obj().p("futureId", id), m));
		}
		for (int i = 0; i < PER; i++)
		{
			long id = g.anyLong();
			int type = g.r.nextInt(4);
			String text = str(g);
			Class<?> ec = new Class<?>[] {
					com.seibel.distanthorizons.core.network.exceptions.RateLimitedException.class,
					com.seibel.distanthorizons.core.network.exceptions.RequestOutOfRangeException.class,
					com.seibel.distanthorizons.core.network.exceptions.RequestRejectedException.class,
					com.seibel.distanthorizons.core.network.exceptions.SectionRequiresSplittingException.class }[type];
			ExceptionMessage m = alloc(ExceptionMessage.class);
			setField(m, AbstractTrackableMessage.class, "futureId", id);
			m.exception = (Exception) ec.getDeclaredConstructor(String.class).newInstance(text);
			out.add(msg("exception_" + i, new Obj().p("futureId", id).p("exceptionType", type).p("message", text), m));
		}
		for (int i = 0; i < PER; i++)
		{
			long id = g.anyLong(), pos = g.anyLong();
			String level = str(g);
			Long ts = g.r.nextInt(3) == 0 ? null : Long.valueOf(g.anyLong());
			FullDataSourceRequestMessage m = alloc(FullDataSourceRequestMessage.class);
			setField(m, AbstractTrackableMessage.class, "futureId", id);
			setField(m, FullDataSourceRequestMessage.class, "levelName", level);
			m.sectionPos = pos;
			m.clientTimestamp = ts;
			out.add(msg("data_request_" + i, new Obj().p("futureId", id).p("levelName", level).p("sectionPos", pos).p("clientTimestamp", ts), m));
		}
		for (int i = 0; i < PER; i++)
		{
			long id = g.anyLong();
			boolean has = g.r.nextInt(4) != 0;
			FullDataPayload p = has ? payload(g) : null;
			FullDataSourceResponseMessage m = alloc(FullDataSourceResponseMessage.class);
			setField(m, AbstractTrackableMessage.class, "futureId", id);
			m.payload = p;
			out.add(msg("data_response_" + i, new Obj().p("futureId", id).p("payload", payloadJson(p)), m));
		}
		for (int i = 0; i < PER; i++)
		{
			String level = str(g);
			FullDataPayload p = payload(g);
			FullDataPartialUpdateMessage m = alloc(FullDataPartialUpdateMessage.class);
			setField(m, FullDataPartialUpdateMessage.class, "levelName", level);
			m.payload = p;
			out.add(msg("partial_update_" + i, new Obj().p("levelName", level).p("payload", payloadJson(p)), m));
		}
		for (int i = 0; i < PER; i++)
		{
			byte[] data = new byte[g.r.nextInt(5) == 0 ? 0 : g.r.nextInt(80)];
			g.r.nextBytes(data);
			int bufId = g.anyInt();
			boolean first = g.r.nextBoolean();
			FullDataSplitMessage m = new FullDataSplitMessage(bufId, Unpooled.wrappedBuffer(data).writerIndex(data.length), first);
			out.add(msg("split_" + i, new Obj().p("bufferId", bufId).p("buffer", Json.hex(data)).p("isFirst", first), m));
		}
		// debug-only message: encode crashes only in phase ENCODE, decode always crashes
		for (int i = 0; i < 2; i++)
		{
			CodecCrashMessage m = new CodecCrashMessage(CodecCrashMessage.ECrashPhase.values()[i]);
			out.add(msg("codec_crash_" + i, new Obj().p("crashPhase", i), m));
		}

		// SessionConfig.constrain over random pairs (values frozen through encode/decode)
		for (int i = 0; i < 200; i++)
		{
			Object[] a = setRandomConfig(g.r);
			SessionConfig ca = freeze(new SessionConfig());
			Object[] b = setRandomConfig(g.r);
			SessionConfig cb = freeze(new SessionConfig());
			Obj res = new Obj();
			ca.constrainingConfig = cb;
			res.p("aConstrainedByB", getters(ca));
			ca.constrainingConfig = null;
			cb.constrainingConfig = ca;
			res.p("bConstrainedByA", getters(cb));
			cb.constrainingConfig = null;
			ca.constrainingConfig = cb;
			ByteBuf bb = Unpooled.buffer();
			ca.encode(bb);
			res.p("aConstrainedByB_encoded", hex(bb));
			out.add(Obj.testCase("session_constrain_" + i, new Obj().p("a", Arrays.asList(a)).p("b", Arrays.asList(b)), res));
		}
		return out;
	}

	// -------------------------------------------------------------------------------------------- helpers

	private static final String[] ALPHABET = { "a", "b", "z", "0", "9", "-", "_", ":", "@", " ", "é", "ü", "中", "文", "€", "😀", "minecraft:overworld", "end" };

	private static String str(Gen g)
	{
		int n = g.r.nextInt(6) == 0 ? 0 : g.r.nextInt(30);
		StringBuilder sb = new StringBuilder();
		for (int i = 0; i < n; i++) { sb.append(ALPHABET[g.r.nextInt(g.r.nextInt(4) == 0 ? ALPHABET.length : 10)]); }
		return sb.toString();
	}

	private static String hex(ByteBuf b)
	{
		byte[] arr = new byte[b.readableBytes()];
		b.getBytes(b.readerIndex(), arr);
		return Json.hex(arr);
	}

	private static Obj msg(String name, Obj in, AbstractNetworkMessage m)
	{
		int id = MessageRegistry.INSTANCE.getMessageId(m);
		Obj res = new Obj().p("id", id);
		ByteBuf body = Unpooled.buffer();
		try
		{
			m.encode(body);
		}
		catch (Throwable t)
		{
			return Obj.testCase(name, in, res.p("throws", rootName(t)));
		}
		String bodyHex = hex(body);
		ByteBuf frame = Unpooled.buffer();
		frame.writeShort(ModInfo.PROTOCOL_VERSION);
		frame.writeShort(id);
		frame.writeBytes(body, body.readerIndex(), body.readableBytes());
		res.p("body", bodyHex).p("frame", hex(frame));
		try
		{
			AbstractNetworkMessage d = (AbstractNetworkMessage) unsafe.allocateInstance(m.getClass());
			ByteBuf copy = Unpooled.copiedBuffer(body);
			d.decode(copy);
			ByteBuf re = Unpooled.buffer();
			d.encode(re);
			res.p("roundTrip", hex(re).equals(bodyHex) && !copy.isReadable());
		}
		catch (Throwable t)
		{
			res.p("roundTrip", false).p("decodeThrows", rootName(t));
		}
		return Obj.testCase(name, in, res);
	}

	private static String rootName(Throwable t)
	{
		while (t.getCause() != null) { t = t.getCause(); }
		return t.getClass().getSimpleName() + ":" + t.getMessage();
	}

	private static FullDataPayload payload(Gen g)
	{
		FullDataPayload p = new FullDataPayload();
		p.dtoBufferId = g.anyInt();
		p.beaconBeams = new ArrayList<>();
		int n = g.r.nextInt(5);
		for (int i = 0; i < n; i++)
		{
			p.beaconBeams.add(new BeaconBeamDTO(new DhBlockPos(g.anyInt(), g.r.nextInt(400) - 64, g.anyInt()), new Color(g.r.nextInt(1 << 24))));
		}
		return p;
	}

	private static Object payloadJson(FullDataPayload p)
	{
		if (p == null) { return null; }
		List<Object> beams = new ArrayList<>();
		for (BeaconBeamDTO b : p.beaconBeams) { beams.add(Arrays.asList(b.blockPos.getX(), b.blockPos.getY(), b.blockPos.getZ(), b.color.getRGB() & 0xFFFFFF)); }
		return new Obj().p("dtoBufferId", p.dtoBufferId).p("beams", beams);
	}

	// ---- SessionConfig ----

	// NOTE: must not be a static initialiser: touching Config before Bootstrap.init() would break the bindings
	private static ConfigEntry<?>[] entries() { return new ConfigEntry<?>[] {
			Config.Common.WorldGenerator.generatorPlan, Config.Server.maxGenerationRequestDistance,
			Config.Common.WorldGenerator.generationCenterChunkX, Config.Common.WorldGenerator.generationCenterChunkZ,
			Config.Common.WorldGenerator.generationMaxChunkRadius, Config.Server.generationRequestRateLimit,
			Config.Server.enableRealTimeUpdates, Config.Server.realTimeUpdateDistanceRadiusInChunks,
			Config.Server.synchronizeOnLoad, Config.Server.maxSyncOnLoadRequestDistance,
			Config.Server.syncOnLoadRateLimit, Config.Server.playerBandwidthLimit }; }

	/** sets every SessionConfig-relevant Config entry to a random value and returns the values read back (enum -> ordinal) */
	@SuppressWarnings({ "unchecked", "rawtypes" })
	private static Object[] setRandomConfig(Random r)
	{
		ConfigEntry<?>[] ENTRIES = entries();
		Object[] vals = new Object[ENTRIES.length];
		for (int i = 0; i < ENTRIES.length; i++)
		{
			ConfigEntry e = ENTRIES[i];
			Object v = e.get();
			Object nv;
			if (v instanceof Boolean) { nv = r.nextBoolean(); }
			else if (v instanceof EDhApiGeneratorPlan) { nv = EDhApiGeneratorPlan.values()[r.nextInt(EDhApiGeneratorPlan.values().length)]; }
			else if (r.nextInt(5) == 0) { nv = 0; }
			else { nv = r.nextInt(r.nextBoolean() ? 100 : 5000); }
			e.set(nv);
			Object back = e.get();
			vals[i] = back instanceof Enum ? (Object) ((Enum<?>) back).ordinal() : back;
		}
		return vals;
	}

	private static SessionConfig freeze(SessionConfig live)
	{
		ByteBuf b = Unpooled.buffer();
		live.encode(b);
		SessionConfig frozen = new SessionConfig();
		frozen.decode(b);
		return frozen;
	}

	private static List<Object> getters(SessionConfig c)
	{
		return Arrays.asList(c.getGeneratorPlan().ordinal(), c.getMaxGenerationRequestDistance(), c.getGenerationCenterChunkX(), c.getGenerationCenterChunkZ(),
				c.getGenerationMaxChunkRadius(), c.getGenerationRequestRateLimit(), c.isRealTimeUpdatesEnabled(), c.getMaxUpdateDistanceRadius(),
				c.getSynchronizeOnLoad(), c.getMaxSyncOnLoadDistance(), c.getSyncOnLoginRateLimit(), c.getPlayerBandwidthLimit());
	}
}
