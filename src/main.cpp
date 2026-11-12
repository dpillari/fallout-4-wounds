// WoundMeshes v0.1
// Attaches meshes\gore\bulletwoundsmall.nif at each bullet impact on an actor,
// parented to the bone that was hit so it follows animation and ragdoll.
//
// Built against libxse/commonlibf4 (supports the current Steam release, 1.11.x).
// Log: Documents\My Games\Fallout4\F4SE\WoundMeshes.log

namespace Config
{
	// Tried in order (the model DB may or may not want the "Meshes\" prefix).
	inline constexpr const char* kModelPaths[] = {
		"Gore\\BulletWoundSmall.nif",
		"Meshes\\Gore\\BulletWoundSmall.nif",
	};

	inline constexpr float       kWoundScale = 1.0f;          // world-space scale
	inline constexpr float       kSinkDepth = 0.25f;          // game units pushed into the surface
	inline constexpr float       kNormalRadialWeight = 0.7f;  // estimated normal: 1 = outward from bone, 0 = toward shooter
	inline constexpr bool        kUseImpactNormal = true;     // prefer the engine's impact normal when it looks valid
	inline constexpr std::size_t kMaxWoundsPerActor = 10;
	inline constexpr std::size_t kMaxActorsTracked = 64;
	inline constexpr bool        kAffectPlayer = false;
	inline constexpr bool        kSkipMelee = true;
	inline constexpr double      kDedupeSeconds = 0.05;
	inline constexpr float       kDedupeDistance = 2.0f;

	// If wounds lie sideways or face into the body, flip this.
	inline constexpr bool kTransposeRotation = false;
}

namespace
{
	// ------------------------------------------------------------------ vector math
	struct V3
	{
		float x{}, y{}, z{};
	};
	V3    operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
	V3    operator-(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
	V3    operator*(V3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
	V3    operator-(V3 a) { return { -a.x, -a.y, -a.z }; }
	float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
	V3    Cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
	float Len(V3 a) { return std::sqrt(Dot(a, a)); }
	bool  IsZero(V3 a) { return Dot(a, a) < 1e-8f; }
	V3    Normalize(V3 a, V3 a_fallback)
	{
		const float l = Len(a);
		return l > 1e-4f ? a * (1.0f / l) : a_fallback;
	}
	V3           ToV3(const RE::NiPoint3& p) { return { p.x, p.y, p.z }; }
	RE::NiPoint3 ToNi(V3 v) { return RE::NiPoint3(v.x, v.y, v.z); }

	// Rotation whose Z axis is the normal, with a random spin around it.
	// Engine convention (CommonLib): v' = M * v, so the columns are the axes.
	RE::NiMatrix3 BasisFromNormal(V3 a_n, float a_roll)
	{
		const V3    z = a_n;
		const V3    helper = std::fabs(z.z) < 0.9f ? V3{ 0, 0, 1 } : V3{ 1, 0, 0 };
		const V3    x0 = Normalize(Cross(helper, z), { 1, 0, 0 });
		const V3    y0 = Cross(z, x0);
		const float c = std::cos(a_roll), s = std::sin(a_roll);
		const V3    x = x0 * c + y0 * s;
		const V3    y = y0 * c - x0 * s;

		RE::NiMatrix3 m;
		const V3      cols[3] = { x, y, z };
		for (int col = 0; col < 3; ++col) {
			m.entry[0][col] = cols[col].x;
			m.entry[1][col] = cols[col].y;
			m.entry[2][col] = cols[col].z;
		}
		return Config::kTransposeRotation ? m.Transpose() : m;
	}

	// DamageImpactData's members are private in CommonLibF4; read them through
	// an identical layout.
	struct ImpactView
	{
		RE::NiPoint3A                           location;  // 00
		RE::NiPoint3A                           normal;    // 10
		RE::NiPoint3A                           velocity;  // 20
		RE::NiPointer<RE::bhkNPCollisionObject> colObj;    // 30
	};
	static_assert(sizeof(ImpactView) == sizeof(RE::DamageImpactData));

	// ------------------------------------------------------------------ state (main thread only)
	struct ActorWounds
	{
		std::deque<RE::NiPointer<RE::NiAVObject>> wounds;
		std::chrono::steady_clock::time_point     lastHit{};
		V3                                        lastPos{};
	};

	std::unordered_map<std::uint32_t, ActorWounds> g_wounds;
	std::unordered_set<const RE::NiAVObject*>      g_ourNodes;
	RE::NiPointer<RE::NiNode>                      g_modelTemplate;
	bool                                           g_modelFailed = false;
	std::mt19937                                   g_rng{ std::random_device{}() };

	// ------------------------------------------------------------------ model loading
	RE::NiNode* GetModelTemplate()
	{
		if (g_modelTemplate || g_modelFailed) {
			return g_modelTemplate.get();
		}

		RE::BSModelDB::DBTraits::ArgsType args{};
		args.loadLevel = 0;
		args.prepareAfterLoad = 1;
		args.performProcess = 1;
		args.loadTextures = 1;
		args.useErrorMarker = 0;
		args.createFadeNode = 0;
		args.faceGenModel = 0;

		for (const auto* path : Config::kModelPaths) {
			RE::NiPointer<RE::NiNode> model;
			const auto                err = RE::BSModelDB::Demand(path, &model, args);
			if (model) {
				REX::INFO("Loaded wound model '{}'", path);
				g_modelTemplate = model;
				return g_modelTemplate.get();
			}
			REX::WARN("Could not load '{}' (error {})", path, static_cast<int>(err));
		}

		REX::ERROR("No wound model could be loaded - wounds disabled");
		g_modelFailed = true;
		return nullptr;
	}

	// Each wound needs its own copy: a node can only have one parent.
	RE::NiPointer<RE::NiAVObject> CreateWoundInstance()
	{
		auto* source = GetModelTemplate();
		if (!source) {
			return {};
		}

		RE::NiCloningProcess cloner{};
		cloner.copyType = RE::NiCloningProcess::CopyType::kCopyExact;
		cloner.appendChar = '$';
		cloner.scale = RE::NiPoint3(1.0f, 1.0f, 1.0f);

		auto* cloneObj = source->CreateClone(cloner);
		if (!cloneObj) {
			REX::ERROR("Cloning the wound model failed");
			return {};
		}
		source->ProcessClone(cloner);

		return RE::NiPointer<RE::NiAVObject>(static_cast<RE::NiAVObject*>(cloneObj));
	}

	// ------------------------------------------------------------------ scene graph helpers
	bool IsUnder(const RE::NiAVObject* a_obj, const RE::NiAVObject* a_root)
	{
		for (const RE::NiAVObject* p = a_obj; p; p = p->parent) {
			if (p == a_root) {
				return true;
			}
		}
		return false;
	}

	template <class F>
	void ForEachNode(RE::NiAVObject* a_obj, F& a_fn)
	{
		if (!a_obj || g_ourNodes.contains(a_obj)) {
			return;
		}
		if (auto* node = a_obj->IsNode()) {
			a_fn(node);
			for (auto& child : node->children) {
				ForEachNode(child.get(), a_fn);
			}
		}
	}

	RE::NiNode* BoneFromHitObject(RE::NiAVObject* a_hit, RE::NiAVObject* a_root)
	{
		if (!a_hit || !IsUnder(a_hit, a_root)) {
			return nullptr;
		}
		if (auto* node = a_hit->IsNode()) {
			return node;
		}
		return a_hit->parent;
	}

	// Nearest node, preferring ragdoll bones (they carry collision objects).
	RE::NiNode* NearestBone(RE::NiAVObject* a_root, V3 a_p)
	{
		RE::NiNode* bestCol = nullptr;
		RE::NiNode* bestAny = nullptr;
		float       dCol = FLT_MAX;
		float       dAny = FLT_MAX;

		auto visit = [&](RE::NiNode* a_node) {
			if (a_node == a_root) {
				return;
			}
			const V3    d = ToV3(a_node->world.translate) - a_p;
			const float dist = Dot(d, d);
			if (a_node->collisionObject && dist < dCol) {
				dCol = dist;
				bestCol = a_node;
			}
			if (dist < dAny) {
				dAny = dist;
				bestAny = a_node;
			}
		};
		ForEachNode(a_root, visit);
		return bestCol ? bestCol : bestAny;
	}

	// Outward direction from the bone's axis (bone -> nearest child bone),
	// blended with "toward the shooter".
	V3 EstimateNormal(RE::NiNode* a_bone, V3 a_p, V3 a_towardShooter)
	{
		const V3 a = ToV3(a_bone->world.translate);
		V3       closest = a;
		float    best = FLT_MAX;

		for (auto& c : a_bone->children) {
			auto* child = c ? c->IsNode() : nullptr;
			if (!child || g_ourNodes.contains(child)) {
				continue;
			}
			const V3    b = ToV3(child->world.translate);
			const V3    ab = b - a;
			const float len2 = Dot(ab, ab);
			if (len2 < 1e-4f) {
				continue;
			}
			const float t = std::clamp(Dot(a_p - a, ab) / len2, 0.0f, 1.0f);
			const V3    cand = a + ab * t;
			const V3    d = a_p - cand;
			const float dist = Dot(d, d);
			if (dist < best) {
				best = dist;
				closest = cand;
			}
		}

		const V3 radial = Normalize(a_p - closest, a_towardShooter);
		const float w = Config::kNormalRadialWeight;
		return Normalize(radial * w + a_towardShooter * (1.0f - w), radial);
	}

	void DetachWound(RE::NiAVObject* a_wound)
	{
		g_ourNodes.erase(a_wound);
		if (auto* parent = a_wound->parent) {
			parent->DetachChild(a_wound);
		}
	}

	void PruneActors()
	{
		if (g_wounds.size() <= Config::kMaxActorsTracked) {
			return;
		}
		// Forget actors whose wounds are no longer attached (their 3D unloaded).
		for (auto it = g_wounds.begin(); it != g_wounds.end();) {
			auto&      list = it->second.wounds;
			const bool gone = std::all_of(list.begin(), list.end(),
				[](const auto& a_w) { return !a_w || !a_w->parent; });
			if (gone) {
				for (auto& w : list) {
					g_ourNodes.erase(w.get());
				}
				it = g_wounds.erase(it);
			} else {
				++it;
			}
		}
	}

	// ------------------------------------------------------------------ the work (main thread)
	struct PendingHit
	{
		V3 pos;
		V3 impactNormal;
		V3 shotDir;
	};

	void SpawnWound(RE::Actor* a_actor, const PendingHit& a_hit, RE::NiAVObject* a_hitObject)
	{
		if (!a_actor) {
			REX::INFO("[spawn] skipped: actor gone");
			return;
		}
		REX::INFO("[spawn] task running");
		auto* root = a_actor->Get3D();
		if (!root) {
			REX::INFO("[spawn] {:08X}: skipped, actor has no 3D", a_actor->GetFormID());
			return;
		}

		const auto formID = a_actor->GetFormID();
		auto&      entry = g_wounds[formID];

		// One bullet can raise more than one hit event.
		const auto now = std::chrono::steady_clock::now();
		if (std::chrono::duration<double>(now - entry.lastHit).count() < Config::kDedupeSeconds &&
			Len(a_hit.pos - entry.lastPos) < Config::kDedupeDistance) {
			REX::INFO("[spawn] {:08X}: skipped, duplicate of previous hit", formID);
			return;
		}
		entry.lastHit = now;
		entry.lastPos = a_hit.pos;

		const char* boneSource = "hit body";
		RE::NiNode* bone = BoneFromHitObject(a_hitObject, root);
		if (!bone) {
			bone = NearestBone(root, a_hit.pos);
			boneSource = "nearest";
		}
		if (!bone) {
			REX::WARN("{:08X}: no bone found", formID);
			return;
		}

		auto wound = CreateWoundInstance();
		if (!wound) {
			return;
		}

		// Surface normal.
		const V3    towardShooter = Normalize(-a_hit.shotDir, { 0, 0, 1 });
		V3          normal = EstimateNormal(bone, a_hit.pos, towardShooter);
		const char* normalSource = "estimated";
		if (Config::kUseImpactNormal && !IsZero(a_hit.impactNormal)) {
			const V3 n = Normalize(a_hit.impactNormal, normal);
			if (Dot(n, towardShooter) > 0.0f) {
				normal = n;
				normalSource = "impact";
			}
		}

		// Desired world transform, converted into the bone's space.
		std::uniform_real_distribution<float> rollDist(0.0f, 6.2831853f);
		RE::NiTransform                       desired;
		desired.rotate = BasisFromNormal(normal, rollDist(g_rng));
		desired.translate = ToNi(a_hit.pos - normal * Config::kSinkDepth);
		desired.scale = Config::kWoundScale;

		wound->local = bone->world.Invert() * desired;

		bone->AttachChild(wound.get(), true);
		RE::NiUpdateData updateData{};
		wound->UpdateDownwardPass(updateData, 0);

		g_ourNodes.insert(wound.get());
		entry.wounds.push_back(wound);
		while (entry.wounds.size() > Config::kMaxWoundsPerActor) {
			if (auto& oldest = entry.wounds.front()) {
				DetachWound(oldest.get());
			}
			entry.wounds.pop_front();
		}
		PruneActors();

		REX::INFO("Wound on {:08X} at ({:.1f}, {:.1f}, {:.1f}) bone '{}' ({}), normal {} - {} on actor",
			formID, a_hit.pos.x, a_hit.pos.y, a_hit.pos.z,
			bone->name.c_str(), boneSource, normalSource, entry.wounds.size());
	}

	// ------------------------------------------------------------------ hit event
	class HitSink final :
		public RE::BSTEventSink<RE::TESHitEvent>
	{
	public:
		static HitSink* GetSingleton()
		{
			static HitSink singleton;
			return &singleton;
		}

		RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent& a_event, RE::BSTEventSource<RE::TESHitEvent>*) override
		{
			auto* targetRef = a_event.target.get();
			auto* causeRef = a_event.cause.get();
			const auto& impact = reinterpret_cast<const ImpactView&>(a_event.hitData.impactData);

			// DIAGNOSTIC: log every hit we receive.
			REX::INFO("[hit] target {:08X} cause {:08X} usesHitData {} proj {:08X} src {:08X} flags {:X} "
					  "loc ({:.1f}, {:.1f}, {:.1f}) nrm ({:.2f}, {:.2f}, {:.2f}) vel ({:.1f}, {:.1f}, {:.1f}) colObj {}",
				targetRef ? targetRef->GetFormID() : 0u, causeRef ? causeRef->GetFormID() : 0u,
				a_event.usesHitData, a_event.projectileFormID, a_event.sourceFormID,
				a_event.hitData.flags.underlying(),
				impact.location.x, impact.location.y, impact.location.z,
				impact.normal.x, impact.normal.y, impact.normal.z,
				impact.velocity.x, impact.velocity.y, impact.velocity.z,
				static_cast<bool>(impact.colObj));

			auto* actor = targetRef ? targetRef->As<RE::Actor>() : nullptr;
			if (!actor) {
				REX::INFO("[hit]   skipped: target is not an actor");
				return RE::BSEventNotifyControl::kContinue;
			}
			if (!Config::kAffectPlayer && actor == RE::PlayerCharacter::GetSingleton()) {
				REX::INFO("[hit]   skipped: target is the player");
				return RE::BSEventNotifyControl::kContinue;
			}
			if (Config::kSkipMelee && a_event.hitData.flags.any(RE::HitData::Flag::kMeleeAttack, RE::HitData::Flag::kBash)) {
				REX::INFO("[hit]   skipped: melee");
				return RE::BSEventNotifyControl::kContinue;
			}

			PendingHit hit{};
			hit.pos = ToV3(impact.location);
			hit.impactNormal = ToV3(impact.normal);
			hit.shotDir = Normalize(ToV3(impact.velocity), { 0, 0, 0 });
			if (IsZero(hit.pos)) {
				REX::INFO("[hit]   skipped: no impact position (usesHitData {})", a_event.usesHitData);
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::NiAVObject* hitObject = impact.colObj ? impact.colObj->GetSceneGraphObject() : nullptr;

			// Scene-graph edits must happen on the main thread; hold references
			// so nothing is freed before the task runs.
			RE::NiPointer<RE::Actor>      actorRef(actor);
			RE::NiPointer<RE::NiAVObject> hitRef(hitObject);
			REX::INFO("[hit]   queued wound task");
			if (auto* tasks = F4SE::GetTaskInterface()) {
				tasks->AddTask([actorRef, hitRef, hit]() {
					SpawnWound(actorRef.get(), hit, hitRef.get());
				});
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	void OnF4SEMessage(F4SE::MessagingInterface::Message* a_msg)
	{
		if (!a_msg || a_msg->type != F4SE::MessagingInterface::kGameDataReady) {
			return;
		}
		if (auto* source = RE::TESHitEvent::GetEventSource()) {
			source->RegisterSink(HitSink::GetSingleton());
			REX::INFO("Hit event sink registered");
		} else {
			REX::ERROR("TESHitEvent source not found");
		}
	}
}

F4SE_PLUGIN_LOAD(const F4SE::LoadInterface* a_f4se)
{
	F4SE::Init(a_f4se);

	REX::INFO("WoundMeshes v0.1 loaded");

	if (auto* messaging = F4SE::GetMessagingInterface()) {
		messaging->RegisterListener(OnF4SEMessage);
	} else {
		REX::ERROR("Messaging interface unavailable");
		return false;
	}
	return true;
}
