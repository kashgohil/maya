// The page capture.mjs drives: one view at a time, rendered by the Khronos glTF Sample Renderer.
import { GltfView, GltfState } from '@khronosgroup/gltf-viewer';

// Renders one view and returns its RGBA pixels, rows bottom up.
window.renderView = async (o) => {
    const canvas = document.getElementById('c');
    canvas.width = o.width;
    canvas.height = o.height;
    if (!window.viewer) {
        const gl = canvas.getContext('webgl2', { antialias: false, alpha: false, preserveDrawingBuffer: true });
        const view = new GltfView(gl);
        const state = view.createState();
        const loader = view.createResourceLoader(undefined, undefined, 'libs/');
        window.viewer = { gl, view, state, loader, model: null };
        state.environment = await loader.loadEnvironment(o.hdr);
    }
    const { view, state, loader } = window.viewer;
    if (window.viewer.model !== o.model) {
        state.gltf = await loader.loadGltf(o.model);
        state.sceneIndex = state.gltf.scene ?? 0;
        window.viewer.model = o.model;
    }
    const p = state.renderingParameters;
    p.toneMap = GltfState.ToneMaps.KHR_PBR_NEUTRAL;
    p.exposure = o.exposure;
    p.usePunctual = false;
    p.useIBL = true;
    p.iblIntensity = 1.0;
    p.renderEnvironmentMap = o.background;
    p.blurEnvironmentMap = false;
    p.environmentRotation = o.rotation;
    p.clearColor = [0, 0, 0, 255];
    state.cameraIndex = undefined;
    state.userCamera.resetView(state.gltf, state.sceneIndex); // clip planes from the scene's bounds
    state.userCamera.setVerticalFoV(o.yfov);
    // The camera's world transform, looking from `from` at `to` with +Y up (gl-matrix's targetTo):
    // UserCamera.lookAt stores a view matrix in its place.
    const sub = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
    const norm = (a) => { const l = Math.hypot(a[0], a[1], a[2]); return [a[0] / l, a[1] / l, a[2] / l]; };
    const cross = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
    const z = norm(sub(o.from, o.to)), x = norm(cross([0, 1, 0], z)), y = cross(z, x);
    state.userCamera.transform = new Float32Array([x[0], x[1], x[2], 0, y[0], y[1], y[2], 0, z[0], z[1], z[2], 0, o.from[0], o.from[1], o.from[2], 1]);
    for (let i = 0; i < 4; ++i) view.renderFrame(state, o.width, o.height); // textures finish uploading
    // The pixels as rendered, read back rather than through the browser's PNG encoder; rows bottom up.
    const gl = window.viewer.gl;
    const pixels = new Uint8Array(o.width * o.height * 4);
    gl.readPixels(0, 0, o.width, o.height, gl.RGBA, gl.UNSIGNED_BYTE, pixels);
    return Array.from(pixels);
};
window.ready = true;
