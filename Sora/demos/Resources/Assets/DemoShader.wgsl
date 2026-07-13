@vertex fn vs_main() -> @builtin(position) vec4f {
    return vec4f(0.0, 0.0, 0.0, 1.0);
}

@fragment fn fs_main() -> @location(0) vec4f {
    return vec4f(0.8, 0.35, 0.16, 1.0);
}