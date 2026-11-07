package dev.kaan.uassetpreview;

import java.util.Map;

/** Small, deterministic explanations. Never guesses gameplay intent from a property name. */
final class AssetKnowledge {
    private static final Map<String, String> TERMS =
            Map.ofEntries(
                    Map.entry(
                            "RelativeLocation",
                            "Position relative to the parent component, measured in centimeters."
                                + " Moving a parent also moves its attached children."),
                    Map.entry(
                            "RelativeRotation",
                            "Orientation relative to the parent, measured in degrees. Pitch tilts"
                                + " up or down, yaw turns left or right, and roll tilts sideways."),
                    Map.entry(
                            "RelativeScale3D",
                            "Size multiplier on each axis. A value of 1 keeps the original size; 2"
                                + " doubles it. Scale also affects attached children."),
                    Map.entry(
                            "StaticMesh",
                            "The 3D shape used by this component. Its material controls the surface"
                                + " appearance separately."),
                    Map.entry(
                            "SkeletalMesh",
                            "A 3D shape with a skeleton for animation. This preview shows its"
                                + " reference pose."),
                    Map.entry(
                            "OverrideMaterials",
                            "The material assigned to a surface slot. Materials control the surface"
                                + " appearance, such as color, roughness and transparency."),
                    Map.entry(
                            "Mobility",
                            "Whether Unreal treats this component as fixed, partly dynamic, or"
                                + " movable during gameplay. This can affect lighting and"
                                + " performance."),
                    Map.entry(
                            "CollisionEnabled",
                            "Whether this component participates in overlap queries, physical"
                                + " collisions, both, or neither."),
                    Map.entry(
                            "CollisionProfileName",
                            "A named set of collision rules describing which objects this component"
                                + " blocks, overlaps, or ignores."),
                    Map.entry(
                            "bGenerateOverlapEvents",
                            "Whether this component can report overlap events when objects enter or"
                                + " leave its collision shape."),
                    Map.entry(
                            "bSimulatePhysics",
                            "Whether Unreal's physics simulation can control this component."
                                + " Physics is not simulated in this preview."),
                    Map.entry(
                            "bEnableGravity",
                            "Whether gravity affects this component when physics simulation is"
                                + " active."),
                    Map.entry(
                            "MassInKgOverride",
                            "The mass used by physics, in kilograms, when a mass override is"
                                + " enabled."),
                    Map.entry(
                            "MaxWalkSpeed",
                            "The movement component's maximum walking speed, in centimeters per"
                                + " second. This setting alone does not describe every force or"
                                + " ability affecting movement."),
                    Map.entry(
                            "JumpZVelocity",
                            "The upward speed applied at the start of a jump, in centimeters per"
                                + " second. Gravity and other movement rules also affect the"
                                + " resulting jump."),
                    Map.entry(
                            "AirControl",
                            "How much directional input can influence movement while falling."
                                + " Custom movement code or abilities can change the effective"
                                + " behavior."),
                    Map.entry(
                            "GravityScale",
                            "A multiplier applied to the movement component's gravity."),
                    Map.entry(
                            "FieldOfView",
                            "The camera's field of view, in degrees. Larger values show a wider"
                                + " view."),
                    Map.entry(
                            "CapsuleRadius",
                            "The radius of the capsule-shaped collision volume, in centimeters."),
                    Map.entry(
                            "CapsuleHalfHeight",
                            "Half the total height of the capsule collision volume, including its"
                                + " rounded ends, in centimeters."),
                    Map.entry(
                            "bHiddenInGame",
                            "Whether this component is hidden during gameplay. Editor visibility"
                                + " and gameplay visibility can differ."),
                    Map.entry(
                            "CastShadow",
                            "Whether this component casts a shadow when its rendering and lighting"
                                + " settings allow it."));

    static String asset(String type) {
        return switch (type) {
            case "Blueprint" ->
                    "A Blueprint defines a reusable class, its default settings and visual"
                        + " scripting logic. Actor Blueprints can also contain components.";
            case "StaticMesh" ->
                    "A Static Mesh is a reusable 3D shape, such as a wall, prop or piece of"
                        + " scenery.";
            case "SkeletalMesh" ->
                    "A Skeletal Mesh is a 3D shape with a skeleton, commonly used for animated"
                        + " characters.";
            case "Texture2D" ->
                    "A Texture is an image that can provide color, surface detail or other data to"
                        + " a material.";
            default ->
                    "This asset stores reusable settings or resources for the Unreal project. Its"
                        + " editable defaults are shown below.";
        };
    }

    static String explain(String property, String tooltip, String category) {
        String known = TERMS.get(property);
        if (known != null) return known;
        if (tooltip != null && !tooltip.isBlank()) return tooltip.strip();
        return "An editable setting in the “"
                + category
                + "” category. This view shows saved defaults; runtime code and scripts may change"
                + " the effective value.";
    }

    private AssetKnowledge() {}
}
