using System.IO;
using ResetFpsBooster.Core.Models;

namespace ResetFpsBooster.Services;

public sealed class GameAutoexecService : IGameAutoexecService
{
    private readonly IBackupService _backupService;
    private readonly IGameLibraryService _libraryService;
    private readonly List<GameAutoexecDefinition> _definitions;

    public GameAutoexecService(IBackupService backupService, IGameLibraryService libraryService)
    {
        _backupService = backupService;
        _libraryService = libraryService;
        _definitions = BuildDefinitions();
    }

    public bool SupportsAutoexec(GameProfile profile) => FindDefinition(profile) is not null;

    public string? GetSupportedGameName(GameProfile profile) => FindDefinition(profile)?.DisplayName;

    public string? GetSteamLaunchOption(GameProfile profile) => FindDefinition(profile)?.LaunchOption;

    public Task<(bool Success, string Message)> ApplyAsync(GameProfile profile, CancellationToken ct = default)
    {
        var definition = FindDefinition(profile);
        if (definition is null)
            return Task.FromResult((false, $"No performance autoexec is available for {profile.Name} yet."));

        if (!Directory.Exists(profile.InstallPath))
            return Task.FromResult((false, $"Could not find the {definition.DisplayName} install folder. Rescan or point the game manually and try again."));

        var cfgFolder = Path.Combine(profile.InstallPath, definition.CfgRelativeFolder);

        try
        {
            Directory.CreateDirectory(cfgFolder);
            var targetPath = Path.Combine(cfgFolder, "autoexec.cfg");

            var fileExisted = File.Exists(targetPath);
            var fileBackup = new FileContentBackup
            {
                TargetPath = targetPath,
                FileExisted = fileExisted,
                PreviousContentBase64 = fileExisted ? Convert.ToBase64String(File.ReadAllBytes(targetPath)) : null
            };

            File.WriteAllText(targetPath, definition.Content);

            var snapshot = _backupService.CommitFileSnapshot(
                $"{definition.DisplayName} Autoexec", $"{definition.DisplayName} FPS Autoexec — {profile.Name}", new[] { fileBackup });

            profile.IsAutoexecApplied = true;
            profile.AutoexecAppliedAt = DateTime.Now;
            profile.AutoexecSnapshotId = snapshot.Id;
            _libraryService.Save(profile);

            return Task.FromResult((true,
                $"Autoexec installed. Add \"{definition.LaunchOption}\" to {definition.DisplayName}'s Steam launch options so it actually loads on startup."));
        }
        catch (Exception ex)
        {
            return Task.FromResult((false, $"Could not write autoexec.cfg: {ex.Message}"));
        }
    }

    public (bool Success, string Message) Restore(GameProfile profile)
    {
        if (string.IsNullOrEmpty(profile.AutoexecSnapshotId))
            return (false, "No autoexec has been installed for this game yet.");

        var (success, message) = _backupService.RestoreSnapshot(profile.AutoexecSnapshotId);

        if (success)
        {
            profile.IsAutoexecApplied = false;
            profile.AutoexecSnapshotId = null;
            _libraryService.Save(profile);
        }

        return (success, message);
    }

    private GameAutoexecDefinition? FindDefinition(GameProfile profile) => _definitions.FirstOrDefault(d => d.Matches(profile));

    private static bool ExecutableIs(GameProfile profile, string fileName) =>
        !string.IsNullOrEmpty(profile.ExecutablePath) && string.Equals(Path.GetFileName(profile.ExecutablePath), fileName, StringComparison.OrdinalIgnoreCase);

    private sealed record GameAutoexecDefinition(
        string DisplayName,
        Func<GameProfile, bool> Matches,
        string CfgRelativeFolder,
        string LaunchOption,
        string Content);

    private static List<GameAutoexecDefinition> BuildDefinitions() => new()
    {
        new GameAutoexecDefinition(
            DisplayName: "Apex Legends",
            Matches: p => ExecutableIs(p, "r5apex.exe") || p.Name.Contains("Apex Legends", StringComparison.OrdinalIgnoreCase),
            CfgRelativeFolder: "cfg",
            LaunchOption: "+exec autoexec",
            Content: ApexAutoexecContent),

        new GameAutoexecDefinition(
            DisplayName: "Counter-Strike 2",
            Matches: p => ExecutableIs(p, "cs2.exe") || p.Name.Contains("Counter-Strike", StringComparison.OrdinalIgnoreCase),
            CfgRelativeFolder: Path.Combine("game", "csgo", "cfg"),
            LaunchOption: "+exec autoexec.cfg",
            Content: Cs2AutoexecContent),
    };

    // User-supplied autoexec (a widely-circulated, community-maintained Apex config), deduplicated
    // and with one broken line fixed (a missing closing quote on mp_usehwmvcds that would have
    // stopped the file parsing partway through). Unlike the earlier hand-picked set, this one goes
    // beyond pure rendering performance into gameplay-feel and audio-balance tweaks (e.g. lowering
    // your own footstep volume, raising enemy footstep audibility, matchmaking/netcode timers) —
    // used here because the user explicitly asked for this exact config, not the more conservative
    // one this module shipped with before.
    private const string ApexAutoexecContent = """
        // ============================================================
        // Managed by RESET FPS BOOSTER — safe to delete or use Restore
        // in the app to revert to the file that was here before.
        // ============================================================

        bind_US_standard "F12" "exec autoexec"
        fps_max 141
        mat_letterbox_aspect_goal 1.6
        mat_letterbox_aspect_threshold 1.6
        building_cubemaps "1"
        cl_fovScale "1.7"
        ai_expression_frametime "0"
        anim_3wayblend "0"
        bink_materials_enabled "0"
        bink_preload_videopanel_movies "0"
        cl_SetupAllBones "0"
        cl_aggregate_particles "1"
        cl_allowAnimsToInterpolateBackward "0"
        cl_always_ragdoll_radius "0"
        cl_anim_detail_dist "1"
        cl_anim_face_dist "1"
        cl_bones_incremental_blend "1"
        cl_cull_weapon_fx "0"
        cl_decal_alwayswhite "1"
        cl_detaildist "0"
        cl_detailfade "0"
        cl_disable_ragdolls "1"
        cl_drawmonitors "0"
        cl_drawshadowtexture "0"
        cl_ejectbrass "0"
        cl_forcepreload "0"
        cl_gib_allow "0"
        cl_idealpitchscale "0"
        cl_jiggle_bone_framerate_cutoff "0"
        cl_lagcompensation "1"
        cl_minimal_rtt_shadows "1"
        cl_muzzleflash_dlight_st "0"
        cl_new_impact_effects "0"
        cl_particle_fallback_base "-1"
        cl_particle_fallback_multiplier "-1"
        cl_particle_limiter_max_particle_count "10"
        cl_particle_limiter_max_system_count "10"
        cl_particle_limiter_min_kill_distance "1"
        cl_particle_max_count "0"
        cl_particle_snoozetime "0.166667"
        cl_phys_maxticks "0"
        cl_phys_props_enable "0"
        cl_predict "1"
        cl_predictweapons "1"
        cl_ragdoll_collide "0"
        cl_ragdoll_force_fade_time "0"
        cl_ragdoll_force_fade_time_local_view_player "0"
        cl_ragdoll_force_fade_time_on_moving_geo "0"
        cl_ragdoll_maxcount "0"
        cl_ragdoll_self_collision "0"
        cl_show_splashes "0"
        cl_showfiredbullets "0"
        cl_showpos "0"
        cl_simdbones_slerp "1"
        cl_smooth "0"
        cl_threaded_bone_setup "1"
        cl_use_simd_bones "1"
        csm_cascade_res "0"
        csm_coverage "0"
        csm_enabled "0"
        csm_quality_level "0"
        csm_renderable_shadows "0"
        csm_rope_shadows "0"
        csm_world_shadows "0"
        disp_dynamic "0"
        dlight_enable "0"
        dodge_viewTiltMax "0"
        dof_enable "0"
        dof_overrideParams "0"
        dvs_enable "0"
        engine_no_focus_sleep "0"
        env_lightglow "0"
        flex_rules "0"
        flex_smooth "0"
        fog_enable "0"
        fog_enable_water_fog "0"
        fog_enableskybox "0"
        fog_volume "0"
        func_break_max_pieces "0"
        g_ragdoll_fadespeed "10000"
        g_ragdoll_lvfadespeed "10000"
        host_sleep "0"
        host_threaded_sound "0"
        hud_setting_adsDof "0"
        hud_setting_minimapRotate "1"
        hud_setting_pingAlpha "0.400000"
        hud_setting_pingDoubleTapEnemy "1"
        hudchat_new_message_fade_duration "1"
        lightmap_ambient "0"
        lightmap_realtimelight "0"
        lightmap_realtimeshadows "0"
        m_acceleration "0"
        map_settings_override "1"
        mat_antialias "0"
        mat_antialias_mode "0"
        mat_autoexposure_override_min_max "1"
        mat_backbuffer_count "0"
        mat_bloom_max_lighting_value "0"
        mat_bloom_streak_amount "0"
        mat_bloom_wide_amount "0"
        mat_bloomscale "0"
        mat_bumpmap "0"
        mat_colcorrection_disableentities "1"
        mat_colorcorrection "0"
        mat_colorcorrection_editor "0"
        mat_compressedtextures "1"
        mat_debug_tonemapping_disable "1"
        mat_depthbias_shadowmap "0"
        mat_depthbias_tightshadowmap "0"
        mat_depthfeather_enable "0"
        mat_depthtest_force_disabled "1"
        mat_diffuse "1"
        mat_disable_bloom "1"
        mat_disable_lightmap_ambient "1"
        mat_disable_lightmaps "1"
        mat_disable_lightwarp "1"
        mat_disable_model_ambient "1"
        mat_dof_enabled "0"
        mat_dynamic_tonemapping "0"
        mat_enable_ssr "0"
        mat_envmap_scale "5"
        mat_envmapsize "0"
        mat_envmaptgasize "0"
        mat_fastspecular "1"
        mat_filterlightmaps "0"
        mat_filtertextures "0"
        mat_force_bloom "0"
        mat_forceaniso "0"
        mat_fullbright "1"
        mat_fxaa_enable "0"
        mat_global_lighting "0"
        mat_hdr_enabled "0"
        mat_hdr_level "0"
        mat_hide_sun_in_last_cascade "1"
        mat_instancing "1"
        mat_light_edit "1"
        mat_local_contrast_scale_override "0"
        mat_maxframelatency "0"
        mat_mip_linear "0"
        mat_motion_blur_enabled "0"
        mat_motion_blur_falling_intensity "0"
        mat_motion_blur_falling_max "0"
        mat_motion_blur_falling_min "0"
        mat_motion_blur_forward_enabled "0"
        mat_motion_blur_percent_of_screen_max "0"
        mat_motion_blur_rotation_intensity "0"
        mat_motion_blur_strength "0"
        mat_parallaxmap "0"
        mat_picmip "0"
        mat_queue_mode "2"
        mat_reducefillrate "1"
        mat_reduceparticles "1"
        mat_screen_blur_enabled "0"
        mat_screen_blur_override "1"
        mat_shadercount "0"
        mat_shadowstate "0"
        mat_specular "0"
        mat_sun_highlight_size "0"
        mat_use_compressed_hdr_textures "1"
        mat_vignette_enable "0"
        mat_vsync "0"
        mat_vsync_mode "0"
        model_fadeRangeFraction "0"
        modeldecals_forceAllowed "0"
        monitor_mat_sharpen_amount "0"
        mp_decals "0"
        mp_usehwmmodels "-1"
        mp_usehwmvcds "-1"
        muzzleflash_light "0"
        nb_shadow_dist "0"
        not_focus_sleep "9999999999999"
        particle_cpu_level "0"
        particle_dlights_enable "0"
        particle_gpu_level "0"
        pertrianglecollision "0"
        projectile_faketrails "0"
        projectile_filltrails "2"
        prop_active_gib_limit "0"
        pvs_yield "1"
        r_DrawBeams "0"
        r_DrawDisp "0"
        r_PhysPropLighting "0"
        r_WaterDrawReflection "0"
        r_blurmenubg "0"
        r_createmodeldecals "0"
        r_decals "0"
        r_ditherAlpha "0"
        r_ditherFade "0"
        r_drawbatchdecals "0"
        r_drawbrushmodels "0"
        r_drawentities "0"
        r_drawopaquerenderables "0"
        r_drawparticles "0"
        r_drawscreenspaceparticles "0"
        r_drawsky "0"
        r_drawsprites "0"
        r_drawstaticlight "0"
        r_drawstaticprops "0"
        r_drawtranslucentrenderables "0"
        r_drawworld "0"
        r_dynamic "0"
        r_dynamiclighting "0"
        r_fastzreject "-1"
        r_forcecheapwater "1"
        r_jiggle_bones "0"
        r_lightmap "0"
        r_lightstyle "0"
        r_modeldecal_maxtotal "0"
        r_norefresh "1"
        r_particle_lighting_enable "0"
        r_particle_lighting_force "0"
        r_particle_low_res_enable "1"
        r_particle_sim_spike_threshold_ms "0"
        r_particles_cull_all "0"
        r_queued_ropes "1"
        r_rimlight "0"
        r_rootlod "2"
        r_ropetranslucent "0"
        r_shadowrendertotexture "0"
        r_sse_s "0"
        r_threaded_particles "1"
        r_threadeddetailprops "1"
        r_txaaEnabled "0"
        r_updaterefracttexture "0"
        r_updaterefracttexture_allowmultiple "0"
        r_visambient "0"
        r_vismodellighting "0"
        r_visualizetraces "0"
        r_volumetric_lighting_enabled "0"
        r_waterdrawrefraction "0"
        r_waterforceexpensive "0"
        r_waterforcereflectentities "0"
        ragdoll_sleepaftertime "0"
        rope_averagelight "0"
        rope_collide "0"
        rope_rendersolid "0"
        rope_smooth "0"
        rope_solid_minalpha "0"
        rope_solid_minwidth "0.1"
        rope_subdiv "0"
        rope_wind_dist "0"
        rui_overrideVguiTextRendering "1"
        shadow_capable "0"
        shadow_default_filter_size "0"
        shadow_depth_dimen_min "0"
        shadow_depth_upres_factor_max "0"
        shadow_enable "0"
        shadow_filter_maxstep "0"
        shadow_maxdynamic "0"
        shadow_maxspotshadows "0"
        shadow_multisampled "0"
        shake_offsetFactor_human "0"
        showfps_enabled "0"
        showfps_heightpercent "0"
        showfps_mouse_latency "0"
        showfps_smoothtime "0"
        showfps_spinner "0"
        showhitlocation "0"
        showmem_enabled "0"
        shownet_enabled "0"
        showsnapshot_enabled "0"
        sleep_when_meeting_framerate "0"
        sleep_when_meeting_framerate_headroom_ms "0"
        setting.cl_ragdoll_self_collision  "0"
        setting.csm_enabled  "0"
        setting.r_lod_switch_scale  "0.4"
        sort_opaque_meshes "0"
        sprint_view_shake_style "1"
        ssao_blur "0"
        ssao_downsample "0"
        ssao_enabled "0"
        sssss_enable "0"
        staticProp_max_scaled_dist "1500"
        static_shadow "0"
        static_shadow_res "0"
        stream_cache_high_priority_static_models "1"
        stream_cache_preload_from_rpak "1"
        stream_drop_unused "1"
        stream_mips_use_staging_texture "0"
        tf_particles_disable_weather "1"
        tracer_extra "0"
        tsaa_blendfactoroverride "1"
        tsaa_curframeblendamount "0.05"
        tsaa_numsamples "4"
        tweak_light_shadows_every_frame "0"
        viewmodelShake "0"
        viewmodelShake_sourceRollRange "0"
        viewmodel_selfshadow "0"
        violence_ablood "0"
        violence_agibs "0"
        violence_hblood "0"
        violence_hgibs "0"
        vphysics_threadmode "1"
        vsm_ignore_face_planes "1"
        m_rawinput "1"
        m_filter "0"
        m_customaccel "0"
        m_customaccel_exponent "0"
        m_mouseaccel1 "0"
        m_mouseaccel2 "0"
        m_customaccel_max "0"
        hud_setting_enableModWheel "1"
        hud_setting_healthWheelUseOnRelease "1"
        ordnanceSwapSelectCooldown "0"
        sidearmSwapSelectCooldown "0"
        sidearmSwapSelectDoubleTapTime "0"
        fov_disableAbilityScaling "1"
        chroma_enable "0"
        miles_channels "2"
        miles_occlusion "0"
        miles_occlusion_force "0"
        miles_occlusion_partial "0"
        miles_nonactor_occlusion "0"
        sound_num_speakers "2"
        sound_classic_music "0"
        sound_musicReduced "0"
        sound_volume_music_game "0.000000"
        sound_volume_music_lobby "0.000000"
        snd_mixahead "0.05"
        snd_async_fullyasync "1"
        snd_musicvolume "0"
        snd_headphone_pan_exponent "2"
        snd_setmixer PlayerFootsteps vol "0.1"
        cl_footstep_event_max_dist "4000"
        sound_without_focus "0"

        // -------------------------
        // Connection settings
        // -------------------------
        telemetry_client_enable "0"
        telemetry_client_sendInterval "0"
        pin_opt_in "0"
        pin_plat_id "0"
        voice_forcemicrecord "0"
        cl_resend "2"
        cl_cmdbackup "4"
        cl_smoothtime "0.01"
        projectile_prediction "1"
        projectile_predictionErrorCorrectTime "0.1"
        cl_matchmaking_timeout "100"
        cl_ranked_reconnect_timeout "300"
        rate "1280000"
        origin_presense_updateRate "20"
        cl_cmdrate "20"
        cl_updaterate_mp "20"
        host_limitlocal "1"
        net_compresspackets "1"
        net_compresspackets_minsize "128"
        net_maxcleartime "0.020346"
        localClientPlayerCachedLevel "25"
        r_cleardecals "1"
        r_decalstaticprops "0"
        cl_comms_filter "-1"
        gfx_nvnUseLowLatencyBoost "1"
        r_deferred_decals "0"
        r_cullshadowworldmeshes "0"
        cl_gib_attack_dir_scale "0"
        cl_debugClientEntities "0"
        glow_outline_effect_enable "0"
        slide_viewTiltSide "0"
        props_break_max_pieces "0"
        cl_phys_props_max "0"
        r_queued_post_processing "1"
        r_hunkalloclightmaps "0"
        r_maxdlights "0"
        r_lightaverage "0"
        r_maxmodeldecal "0"
        r_drawmodeldecals "0"
        r_eyes "0"
        r_teeth "0"
        r_flex "0"
        projectile_muzzleOffsetFirstPersonDecayMaxTime "0"
        projectile_muzzleOffsetFirstPersonDecayDist "0"
        r_drawtracers_firstperson "0"
        r_shadowmaxrendered "0"
        r_shadows "0"
        shadow_always_update "0"
        mat_postprocess_enable "0"
        cl_fasttempentcollision "20"
        player_disallow_negative_frametime "0"
        r_lod_switch_scale "0.4"
        mat_bloom_scalefactor_scalar "0"
        noise_filter_scale "0"
        nx_static_lobby_mode "2"
        r_particle_timescale "3"
        r_fullscreen "1"
        stream_memory "0"
        mp_usehwmmodels "-1"
        mp_usehwmvcds "-1"
        """;

    // CS2 runs on Source 2, whose exposed client cvars are far more limited than the old Source 1
    // games — most of the classic "mat_*"/"r_*" toggles no longer exist or are handled internally
    // by the renderer. This set is intentionally short: only cvars verified against the actively
    // maintained, community-trusted ArminC-AutoExec project (376+ stars, CC0), and only the ones
    // that are pure engine/rendering performance with no gameplay-feel or competitive side effects
    // (e.g. deliberately excluding netcode-prediction and interpolation cvars, which trade fairness
    // or connection stability for perceived responsiveness).
    private const string Cs2AutoexecContent = """
        // ============================================================
        // Managed by RESET FPS BOOSTER — safe to delete or use Restore
        // in the app to revert to the file that was here before.
        // Performance-only: no sensitivity, audio, or netcode changes.
        // ============================================================

        fps_max 0                          // Uncap the frame rate in-game
        fps_max_ui 60                      // Cap the frame rate in menus — saves GPU/heat for no visible benefit there
        thread_pool_option 2               // Prefer performance cores on hybrid CPUs (Intel 12th-gen+/AMD equivalents)
        """;
}
