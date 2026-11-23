// Fixed-function OpenGL 1.1 subset (as used by Peter's D3GL0/D3GL1 backends) emulated on WebGL2
#include "shim.h"
#include <GLES3/gl3.h>
#include <emscripten/html5.h>
#include <cmath>
#include <algorithm>

typedef double GLclampd;

// legacy GL enums not present in GLES3 headers
#define LGL_QUADS 0x0007
#define LGL_QUAD_STRIP 0x0008
#define LGL_POLYGON 0x0009
#define LGL_MODELVIEW 0x1700
#define LGL_PROJECTION 0x1701
#define LGL_TEXTURE 0x1702
#define LGL_LIGHTING 0x0B50
#define LGL_LIGHT0 0x4000
#define LGL_COLOR_MATERIAL 0x0B57
#define LGL_NORMALIZE 0x0BA1
#define LGL_FOG 0x0B60
#define LGL_ALPHA_TEST 0x0BC0
#define LGL_TEXTURE_GEN_S 0x0C60
#define LGL_TEXTURE_GEN_T 0x0C61
#define LGL_AMBIENT 0x1200
#define LGL_DIFFUSE 0x1201
#define LGL_SPECULAR 0x1202
#define LGL_POSITION 0x1203
#define LGL_SPOT_DIRECTION 0x1204
#define LGL_SPOT_EXPONENT 0x1205
#define LGL_SPOT_CUTOFF 0x1206
#define LGL_CONSTANT_ATTENUATION 0x1207
#define LGL_LINEAR_ATTENUATION 0x1208
#define LGL_QUADRATIC_ATTENUATION 0x1209
#define LGL_EMISSION 0x1600
#define LGL_SHININESS 0x1601
#define LGL_AMBIENT_AND_DIFFUSE 0x1602
#define LGL_LIGHT_MODEL_LOCAL_VIEWER 0x0B51
#define LGL_LIGHT_MODEL_TWO_SIDE 0x0B52
#define LGL_LIGHT_MODEL_AMBIENT 0x0B53
#define LGL_FOG_DENSITY 0x0B62
#define LGL_FOG_START 0x0B63
#define LGL_FOG_END 0x0B64
#define LGL_FOG_MODE 0x0B65
#define LGL_FOG_COLOR 0x0B66
#define LGL_EXP 0x0800
#define LGL_EXP2 0x0801
#define LGL_LINEAR 0x2601
#define LGL_FLAT 0x1D00
#define LGL_SMOOTH 0x1D01
#define LGL_LINE 0x1B01
#define LGL_FILL 0x1B02
#define LGL_TEXTURE_ENV_MODE 0x2200
#define LGL_MODULATE 0x2100
#define LGL_DECAL 0x2101
#define LGL_ADD 0x0104
#define LGL_REPLACE 0x1E01
#define LGL_BLEND_ENV 0x0BE2
#define LGL_VERTEX_ARRAY 0x8074
#define LGL_NORMAL_ARRAY 0x8075
#define LGL_COLOR_ARRAY 0x8076
#define LGL_INDEX_ARRAY 0x8077
#define LGL_TEXTURE_COORD_ARRAY 0x8078
#define LGL_CLAMP 0x2900
#define LGL_STENCIL_BITS 0x0D57
#define LGL_DOUBLE 0x140A

// ------------------------------------------------------------------ state
struct M4 { float m[16]; };
static M4 ident() { M4 r{}; r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1; return r; }
static M4 mul(const M4& a, const M4& b)	// column-major a*b
{
	M4 r;
	for (int c = 0; c < 4; c++) for (int rr = 0; rr < 4; rr++)
	{
		float s = 0; for (int k = 0; k < 4; k++) s += a.m[k * 4 + rr] * b.m[c * 4 + k];
		r.m[c * 4 + rr] = s;
	}
	return r;
}
static void xform(const M4& a, const float* v, float* o)
{
	for (int r = 0; r < 4; r++) o[r] = a.m[r] * v[0] + a.m[4 + r] * v[1] + a.m[8 + r] * v[2] + a.m[12 + r] * v[3];
}

struct Light {
	bool on = false;
	float amb[4] = {0, 0, 0, 1}, dif[4] = {0, 0, 0, 1}, spec[4] = {0, 0, 0, 1};
	float pos[4] = {0, 0, 1, 0};	// eye space
	float spotdir[3] = {0, 0, -1};
	float spotexp = 0, spotcut = 180;
	float att[3] = {1, 0, 0};
};
struct ClientArray { bool on = false; int size = 4; GLenum type = GL_FLOAT; int stride = 0; const void* ptr = nullptr; };

struct TexRec { GLuint tex = 0; int maxLevel = -1; GLint minf = GL_NEAREST_MIPMAP_LINEAR, magf = GL_LINEAR, ws = GL_REPEAT, wt = GL_REPEAT; int w = 0, h = 0; };

struct GLCtx {
	uint32_t magic = 0x474c4358;
	EMSCRIPTEN_WEBGL_CONTEXT_HANDLE h = 0;
	Wnd* wnd = nullptr;
	GLuint fbo = 0, colorRb = 0, depthRb = 0; int fw = 0, fh = 0;
	GLuint prog[2] = {0, 0};
	GLuint vbo = 0, vao = 0;
	std::map<GLuint, TexRec> tex;
	GLuint bound = 0;
	// matrices
	int mode = LGL_MODELVIEW;
	std::vector<M4> mv{ident()}, pj{ident()}, tx{ident()};
	// current vertex attribs
	float col[4] = {1, 1, 1, 1}, nrm[3] = {0, 0, 1}, uv[2] = {0, 0};
	// immediate mode
	bool inBegin = false; GLenum prim = 0; std::vector<float> imm;
	// lighting
	bool lighting = false, colmat = false, twoside = false, localview = false;
	Light L[8];
	float lmamb[4] = {0.2f, 0.2f, 0.2f, 1};
	float mamb[4] = {0.2f, 0.2f, 0.2f, 1}, mdif[4] = {0.8f, 0.8f, 0.8f, 1}, mspec[4] = {0, 0, 0, 1}, memi[4] = {0, 0, 0, 1}; float mshin = 0;
	GLenum colmatMode = LGL_AMBIENT_AND_DIFFUSE;
	// fog
	bool fog = false; GLenum fogmode = LGL_EXP; float fogstart = 0, fogend = 1, fogdens = 1, fogcol[4] = {0, 0, 0, 0};
	// texture
	bool tex2d = false; GLenum texenv = LGL_MODULATE;
	// alpha
	bool alphatest = false; GLenum alphafunc = GL_ALWAYS; float alpharef = 0;
	// misc
	bool flat = false; GLenum polymode = LGL_FILL;
	ClientArray va, na, ca, ta;
	// cached uniform locations
	struct Loc { GLint mv, pj, nm, lighting, lpos[8], lamb[8], ldif[8], lspec[8], lsd[8], lse[8], lsc[8], latt[8], lon[8], lmamb, mamb, mdif, mspec, memi, mshin,
		colmat, twoside, localview, fogmode, fogp, fogcol, tex, texenv, alphafunc, alpharef, sampler; } loc[2];
};
static GLCtx* g_ctx = nullptr;
static HDC g_curDC = NULL;

// ------------------------------------------------------------------ shaders
static const char* VS = R"(#version 300 es
precision highp float;
in vec4 a_pos; in vec3 a_nrm; in vec4 a_col; in vec2 a_tex;
uniform mat4 u_mv, u_pj; uniform mat3 u_nm;
uniform bool u_lighting, u_colmat, u_twoside, u_localview;
uniform vec4 u_lpos[8], u_lamb[8], u_ldif[8], u_lspec[8]; uniform vec3 u_lsd[8]; uniform float u_lse[8], u_lsc[8]; uniform vec3 u_latt[8]; uniform bool u_lon[8];
uniform vec4 u_lmamb, u_mamb, u_mdif, u_mspec, u_memi; uniform float u_mshin;
FLATQ out vec4 v_col; FLATQ out vec4 v_colb; out vec2 v_tex; out float v_fogz;
vec4 light(vec3 V, vec3 N, vec4 amb, vec4 dif) {
  vec4 c = u_memi + u_lmamb * amb;
  for (int i = 0; i < 8; i++) {
    if (!u_lon[i]) continue;
    vec3 L; float att = 1.0;
    if (u_lpos[i].w == 0.0) L = normalize(u_lpos[i].xyz);
    else {
      vec3 d = u_lpos[i].xyz - V; float dist = length(d); L = d / max(dist, 1e-6);
      att = 1.0 / max(u_latt[i].x + u_latt[i].y * dist + u_latt[i].z * dist * dist, 1e-6);
      if (u_lsc[i] < 180.0) {
        float s = dot(-L, normalize(u_lsd[i]));
        att *= (s < cos(radians(u_lsc[i]))) ? 0.0 : pow(max(s, 0.0), u_lse[i]);
      }
    }
    float nl = max(dot(N, L), 0.0);
    vec4 term = u_lamb[i] * amb + nl * u_ldif[i] * dif;
    if (nl > 0.0) {
      vec3 E = u_localview ? normalize(-V) : vec3(0.0, 0.0, 1.0);
      vec3 H = normalize(L + E);
      term += pow(max(dot(N, H), 0.0), max(u_mshin, 0.0001)) * u_lspec[i] * u_mspec;
    }
    c += att * term;
  }
  c.a = dif.a;
  return clamp(c, 0.0, 1.0);
}
void main() {
  vec4 eye = u_mv * a_pos;
  gl_Position = u_pj * eye;
  v_tex = a_tex;
  v_fogz = abs(eye.z / max(abs(eye.w), 1e-6));
  if (u_lighting) {
    vec3 N = normalize(u_nm * a_nrm);
    vec4 amb = u_colmat ? a_col : u_mamb;
    vec4 dif = u_colmat ? a_col : u_mdif;
    vec3 V = eye.xyz / eye.w;
    v_col = light(V, N, amb, dif);
    v_colb = u_twoside ? light(V, -N, amb, dif) : v_col;
  } else { v_col = a_col; v_colb = a_col; }
}
)";
static const char* FS = R"(#version 300 es
precision highp float;
FLATQ in vec4 v_col; FLATQ in vec4 v_colb; in vec2 v_tex; in float v_fogz;
uniform int u_fogmode; uniform vec3 u_fogp; uniform vec4 u_fogcol;
uniform bool u_tex; uniform int u_texenv; uniform sampler2D u_sampler;
uniform int u_alphafunc; uniform float u_alpharef;
out vec4 o;
void main() {
  vec4 c = gl_FrontFacing ? v_col : v_colb;
  if (u_tex) {
    vec4 t = texture(u_sampler, v_tex);
    if (u_texenv == 1) c = t;
    else if (u_texenv == 2) c = vec4(mix(c.rgb, t.rgb, t.a), c.a);
    else if (u_texenv == 3) c = vec4(c.rgb + t.rgb, c.a * t.a);
    else if (u_texenv == 4) c = vec4(c.rgb * (1.0 - t.rgb), c.a * t.a);
    else c = c * t;
  }
  if (u_alphafunc != 0) {
    bool pass = true; float a = c.a, r = u_alpharef;
    if (u_alphafunc == 1) pass = false;
    else if (u_alphafunc == 2) pass = a < r;
    else if (u_alphafunc == 3) pass = a == r;
    else if (u_alphafunc == 4) pass = a <= r;
    else if (u_alphafunc == 5) pass = a > r;
    else if (u_alphafunc == 6) pass = a != r;
    else if (u_alphafunc == 7) pass = a >= r;
    if (!pass) discard;
  }
  if (u_fogmode != 0) {
    float f;
    if (u_fogmode == 1) f = (u_fogp.y - v_fogz) / max(u_fogp.y - u_fogp.x, 1e-6);
    else if (u_fogmode == 2) f = exp(-u_fogp.z * v_fogz);
    else { float d = u_fogp.z * v_fogz; f = exp(-d * d); }
    f = clamp(f, 0.0, 1.0);
    c.rgb = mix(u_fogcol.rgb, c.rgb, f);
  }
  o = c;
}
)";

static GLuint compile(GLenum type, const std::string& src)
{
	GLuint s = glCreateShader(type);
	const char* p = src.c_str(); glShaderSource(s, 1, &p, nullptr); glCompileShader(s);
	GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) { char log[2048]; glGetShaderInfoLog(s, sizeof log, nullptr, log); SHIM_LOG("[gl] shader error: %s", log); }
	return s;
}
static std::string with_flat(const char* src, bool flat)
{
	std::string s = src; std::string q = flat ? "flat" : "";
	size_t p; while ((p = s.find("FLATQ")) != std::string::npos) s.replace(p, 5, q);
	return s;
}
static void build_programs(GLCtx* c)
{
	for (int f = 0; f < 2; f++)
	{
		GLuint p = glCreateProgram();
		glAttachShader(p, compile(GL_VERTEX_SHADER, with_flat(VS, f)));
		glAttachShader(p, compile(GL_FRAGMENT_SHADER, with_flat(FS, f)));
		glBindAttribLocation(p, 0, "a_pos"); glBindAttribLocation(p, 1, "a_nrm"); glBindAttribLocation(p, 2, "a_col"); glBindAttribLocation(p, 3, "a_tex");
		glLinkProgram(p);
		GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
		if (!ok) { char log[2048]; glGetProgramInfoLog(p, sizeof log, nullptr, log); SHIM_LOG("[gl] link error: %s", log); }
		c->prog[f] = p;
		auto& L = c->loc[f];
#define U(n) glGetUniformLocation(p, n)
		L.mv = U("u_mv"); L.pj = U("u_pj"); L.nm = U("u_nm"); L.lighting = U("u_lighting");
		char b[64];
		for (int i = 0; i < 8; i++)
		{
			snprintf(b, 64, "u_lpos[%d]", i); L.lpos[i] = U(b); snprintf(b, 64, "u_lamb[%d]", i); L.lamb[i] = U(b);
			snprintf(b, 64, "u_ldif[%d]", i); L.ldif[i] = U(b); snprintf(b, 64, "u_lspec[%d]", i); L.lspec[i] = U(b);
			snprintf(b, 64, "u_lsd[%d]", i); L.lsd[i] = U(b); snprintf(b, 64, "u_lse[%d]", i); L.lse[i] = U(b);
			snprintf(b, 64, "u_lsc[%d]", i); L.lsc[i] = U(b); snprintf(b, 64, "u_latt[%d]", i); L.latt[i] = U(b);
			snprintf(b, 64, "u_lon[%d]", i); L.lon[i] = U(b);
		}
		L.lmamb = U("u_lmamb"); L.mamb = U("u_mamb"); L.mdif = U("u_mdif"); L.mspec = U("u_mspec"); L.memi = U("u_memi"); L.mshin = U("u_mshin");
		L.colmat = U("u_colmat"); L.twoside = U("u_twoside"); L.localview = U("u_localview");
		L.fogmode = U("u_fogmode"); L.fogp = U("u_fogp"); L.fogcol = U("u_fogcol");
		L.tex = U("u_tex"); L.texenv = U("u_texenv"); L.alphafunc = U("u_alphafunc"); L.alpharef = U("u_alpharef"); L.sampler = U("u_sampler");
#undef U
	}
}

static void ensure_fbo(GLCtx* c)
{
	RECT r = c->wnd->clientRect();
	int w = std::max(1, (int)(r.right - r.left)), h = std::max(1, (int)(r.bottom - r.top));
	if (c->fbo && c->fw == w && c->fh == h) return;
	if (!c->fbo) { glGenFramebuffers(1, &c->fbo); glGenRenderbuffers(1, &c->colorRb); glGenRenderbuffers(1, &c->depthRb); }
	glBindRenderbuffer(GL_RENDERBUFFER, c->colorRb); glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
	glBindRenderbuffer(GL_RENDERBUFFER, c->depthRb); glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
	glBindFramebuffer(GL_FRAMEBUFFER, c->fbo);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, c->colorRb);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, c->depthRb);
	c->fw = w; c->fh = h;
}

// ------------------------------------------------------------------ drawing
static M4& cur_matrix(GLCtx* c) { return c->mode == LGL_PROJECTION ? c->pj.back() : c->mode == LGL_TEXTURE ? c->tx.back() : c->mv.back(); }

static void upload_state(GLCtx* c)
{
	int f = c->flat ? 1 : 0;
	glUseProgram(c->prog[f]);
	auto& L = c->loc[f];
	const M4& mv = c->mv.back();
	glUniformMatrix4fv(L.mv, 1, GL_FALSE, mv.m);
	glUniformMatrix4fv(L.pj, 1, GL_FALSE, c->pj.back().m);
	// normal matrix = inverse transpose of upper 3x3
	float a[9] = {mv.m[0], mv.m[1], mv.m[2], mv.m[4], mv.m[5], mv.m[6], mv.m[8], mv.m[9], mv.m[10]};
	float det = a[0] * (a[4] * a[8] - a[7] * a[5]) - a[3] * (a[1] * a[8] - a[7] * a[2]) + a[6] * (a[1] * a[5] - a[4] * a[2]);
	float nm[9];
	if (fabsf(det) < 1e-12f) { memcpy(nm, a, sizeof nm); }
	else
	{
		float id = 1.0f / det;
		// inverse (column-major), then transpose
		float inv[9];
		inv[0] = (a[4] * a[8] - a[7] * a[5]) * id; inv[3] = -(a[3] * a[8] - a[6] * a[5]) * id; inv[6] = (a[3] * a[7] - a[6] * a[4]) * id;
		inv[1] = -(a[1] * a[8] - a[7] * a[2]) * id; inv[4] = (a[0] * a[8] - a[6] * a[2]) * id; inv[7] = -(a[0] * a[7] - a[6] * a[1]) * id;
		inv[2] = (a[1] * a[5] - a[4] * a[2]) * id; inv[5] = -(a[0] * a[5] - a[3] * a[2]) * id; inv[8] = (a[0] * a[4] - a[3] * a[1]) * id;
		for (int r = 0; r < 3; r++) for (int cc = 0; cc < 3; cc++) nm[cc * 3 + r] = inv[r * 3 + cc];
	}
	glUniformMatrix3fv(L.nm, 1, GL_FALSE, nm);
	glUniform1i(L.lighting, c->lighting);
	if (c->lighting)
	{
		for (int i = 0; i < 8; i++)
		{
			glUniform1i(L.lon[i], c->L[i].on);
			if (!c->L[i].on) continue;
			glUniform4fv(L.lpos[i], 1, c->L[i].pos); glUniform4fv(L.lamb[i], 1, c->L[i].amb); glUniform4fv(L.ldif[i], 1, c->L[i].dif);
			glUniform4fv(L.lspec[i], 1, c->L[i].spec); glUniform3fv(L.lsd[i], 1, c->L[i].spotdir); glUniform1f(L.lse[i], c->L[i].spotexp);
			glUniform1f(L.lsc[i], c->L[i].spotcut); glUniform3fv(L.latt[i], 1, c->L[i].att);
		}
		glUniform4fv(L.lmamb, 1, c->lmamb); glUniform4fv(L.mamb, 1, c->mamb); glUniform4fv(L.mdif, 1, c->mdif);
		glUniform4fv(L.mspec, 1, c->mspec); glUniform4fv(L.memi, 1, c->memi); glUniform1f(L.mshin, c->mshin);
		glUniform1i(L.colmat, c->colmat); glUniform1i(L.twoside, c->twoside); glUniform1i(L.localview, c->localview);
	}
	int fm = 0;
	if (c->fog) fm = c->fogmode == LGL_LINEAR ? 1 : c->fogmode == LGL_EXP2 ? 3 : 2;
	glUniform1i(L.fogmode, fm);
	if (fm) { glUniform3f(L.fogp, c->fogstart, c->fogend, c->fogdens); glUniform4fv(L.fogcol, 1, c->fogcol); }
	bool tex = c->tex2d && c->tex.count(c->bound) && c->tex[c->bound].maxLevel >= 0;
	glUniform1i(L.tex, tex);
	if (tex)
	{
		TexRec& t = c->tex[c->bound];
		glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, t.tex);
		GLint minf = t.minf;
		if (t.maxLevel == 0 && (minf == GL_NEAREST_MIPMAP_NEAREST || minf == GL_NEAREST_MIPMAP_LINEAR)) minf = GL_NEAREST;
		if (t.maxLevel == 0 && (minf == GL_LINEAR_MIPMAP_NEAREST || minf == GL_LINEAR_MIPMAP_LINEAR)) minf = GL_LINEAR;
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minf);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, t.magf);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, t.ws);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, t.wt);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, t.maxLevel);
		glUniform1i(L.sampler, 0);
		int env = 0;
		switch (c->texenv) { case LGL_REPLACE: env = 1; break; case LGL_DECAL: env = 2; break; case LGL_ADD: env = 3; break; case LGL_BLEND_ENV: env = 4; break; default: env = 0; }
		glUniform1i(L.texenv, env);
	}
	int af = 0;
	if (c->alphatest) af = (int)(c->alphafunc - GL_NEVER) + 1;
	if (af == 8) af = 0;	// GL_ALWAYS
	glUniform1i(L.alphafunc, af);
	glUniform1f(L.alpharef, c->alpharef);
}

// vertex layout: pos4 nrm3 col4 tex2 = 13 floats
static void draw_tris(GLCtx* c, std::vector<float>& v, GLenum mode)
{
	if (v.empty()) return;
	ensure_fbo(c);
	glBindFramebuffer(GL_FRAMEBUFFER, c->fbo);
	upload_state(c);
	glBindVertexArray(c->vao);
	glBindBuffer(GL_ARRAY_BUFFER, c->vbo);
	glBufferData(GL_ARRAY_BUFFER, v.size() * 4, v.data(), GL_STREAM_DRAW);
	int n = (int)v.size() / 13;
	if (c->polymode == LGL_LINE && mode == GL_TRIANGLES)
	{
		std::vector<float> l; l.reserve(v.size() * 2);
		for (int t = 0; t + 2 < n; t += 3)
		{
			int e[6] = {t, t + 1, t + 1, t + 2, t + 2, t};
			for (int k : e) l.insert(l.end(), v.begin() + k * 13, v.begin() + k * 13 + 13);
		}
		glBufferData(GL_ARRAY_BUFFER, l.size() * 4, l.data(), GL_STREAM_DRAW);
		glDrawArrays(GL_LINES, 0, (int)l.size() / 13);
		return;
	}
	glDrawArrays(mode, 0, n);
}

// convert primitive to triangle list (in place on vertex vector)
static void to_triangles(std::vector<float>& v, GLenum prim, std::vector<float>& out, GLenum& mode)
{
	int n = (int)v.size() / 13;
	out.clear(); mode = GL_TRIANGLES;
	auto push = [&](int i) { out.insert(out.end(), v.begin() + i * 13, v.begin() + i * 13 + 13); };
	switch (prim)
	{
	case GL_TRIANGLES: out.swap(v); n = (int)out.size() / 13; out.resize(n / 3 * 3 * 13); return;
	case GL_TRIANGLE_STRIP: for (int i = 2; i < n; i++) { if (i & 1) { push(i - 1); push(i - 2); push(i); } else { push(i - 2); push(i - 1); push(i); } } return;
	case GL_TRIANGLE_FAN: case LGL_POLYGON: for (int i = 2; i < n; i++) { push(0); push(i - 1); push(i); } return;
	case LGL_QUADS: for (int i = 0; i + 3 < n; i += 4) { push(i); push(i + 1); push(i + 2); push(i); push(i + 2); push(i + 3); } return;
	case LGL_QUAD_STRIP: for (int i = 0; i + 3 < n; i += 2) { push(i); push(i + 1); push(i + 3); push(i); push(i + 3); push(i + 2); } return;
	case GL_LINES: case GL_LINE_STRIP: case GL_LINE_LOOP: case GL_POINTS: out.swap(v); mode = prim; return;
	}
}

static void emit_vertex(GLCtx* c, float x, float y, float z, float w)
{
	float vx[13] = {x, y, z, w, c->nrm[0], c->nrm[1], c->nrm[2], c->col[0], c->col[1], c->col[2], c->col[3], c->uv[0], c->uv[1]};
	c->imm.insert(c->imm.end(), vx, vx + 13);
}

static float read_comp(const ClientArray& a, int idx, int k)
{
	int esz = a.type == GL_FLOAT ? 4 : a.type == LGL_DOUBLE ? 8 : a.type == GL_UNSIGNED_BYTE || a.type == GL_BYTE ? 1 : a.type == GL_SHORT || a.type == GL_UNSIGNED_SHORT ? 2 : 4;
	int stride = a.stride ? a.stride : esz * a.size;
	const BYTE* p = (const BYTE*)a.ptr + (size_t)idx * stride + k * esz;
	switch (a.type)
	{
	case GL_FLOAT: return *(const float*)p;
	case LGL_DOUBLE: return (float)*(const double*)p;
	case GL_UNSIGNED_BYTE: return *p / 255.0f;
	case GL_BYTE: return *(const signed char*)p / 127.0f;
	case GL_SHORT: return *(const short*)p;
	case GL_UNSIGNED_SHORT: return *(const unsigned short*)p;
	case GL_INT: return (float)*(const int*)p;
	case GL_UNSIGNED_INT: return (float)*(const unsigned*)p;
	}
	return 0;
}
static void emit_array_element(GLCtx* c, int i)
{
	if (c->na.on && c->na.ptr) for (int k = 0; k < 3; k++) c->nrm[k] = read_comp(c->na, i, k);
	if (c->ca.on && c->ca.ptr) { for (int k = 0; k < c->ca.size; k++) c->col[k] = read_comp(c->ca, i, k); if (c->ca.size == 3) c->col[3] = 1; }
	if (c->ta.on && c->ta.ptr) for (int k = 0; k < std::min(2, c->ta.size); k++) c->uv[k] = read_comp(c->ta, i, k);
	if (c->va.on && c->va.ptr)
	{
		float p[4] = {0, 0, 0, 1};
		for (int k = 0; k < c->va.size; k++) p[k] = read_comp(c->va, i, k);
		emit_vertex(c, p[0], p[1], p[2], p[3]);
	}
}

// ------------------------------------------------------------------ GL entry points (WINAPI = cdecl on wasm)
#define CTX GLCtx* c = g_ctx; if (!c) return

static void APIENTRY shgl_Begin(GLenum m) { CTX; c->inBegin = true; c->prim = m; c->imm.clear(); }
static void APIENTRY shgl_End() { CTX; c->inBegin = false; std::vector<float> t; GLenum mode; to_triangles(c->imm, c->prim, t, mode); draw_tris(c, t, mode); c->imm.clear(); }
static void APIENTRY shgl_Vertex2i(GLint x, GLint y) { CTX; emit_vertex(c, (float)x, (float)y, 0, 1); }
static void APIENTRY shgl_Vertex2f(GLfloat x, GLfloat y) { CTX; emit_vertex(c, x, y, 0, 1); }
static void APIENTRY shgl_Vertex3f(GLfloat x, GLfloat y, GLfloat z) { CTX; emit_vertex(c, x, y, z, 1); }
static void APIENTRY shgl_Vertex3fv(const GLfloat* v) { CTX; emit_vertex(c, v[0], v[1], v[2], 1); }
static void APIENTRY shgl_Normal3fv(const GLfloat* v) { CTX; c->nrm[0] = v[0]; c->nrm[1] = v[1]; c->nrm[2] = v[2]; }
static void APIENTRY shgl_TexCoord2fv(const GLfloat* v) { CTX; c->uv[0] = v[0]; c->uv[1] = v[1]; }
static void APIENTRY shgl_Color3f(GLfloat r, GLfloat g, GLfloat b) { CTX; c->col[0] = r; c->col[1] = g; c->col[2] = b; c->col[3] = 1; }
static void APIENTRY shgl_Color3fv(const GLfloat* v) { CTX; c->col[0] = v[0]; c->col[1] = v[1]; c->col[2] = v[2]; c->col[3] = 1; }
static void APIENTRY shgl_Color4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { CTX; c->col[0] = r; c->col[1] = g; c->col[2] = b; c->col[3] = a; }
static void APIENTRY shgl_Color4fv(const GLfloat* v) { CTX; memcpy(c->col, v, 16); }

static void APIENTRY shgl_MatrixMode(GLenum m) { CTX; c->mode = m; }
static void APIENTRY shgl_LoadIdentity() { CTX; cur_matrix(c) = ident(); }
static void APIENTRY shgl_LoadMatrixf(const GLfloat* m) { CTX; memcpy(cur_matrix(c).m, m, 64); }
static void APIENTRY shgl_MultMatrixf(const GLfloat* m) { CTX; M4 b; memcpy(b.m, m, 64); cur_matrix(c) = mul(cur_matrix(c), b); }
static void APIENTRY shgl_PushMatrix() { CTX; auto& s = c->mode == LGL_PROJECTION ? c->pj : c->mode == LGL_TEXTURE ? c->tx : c->mv; s.push_back(s.back()); }
static void APIENTRY shgl_PopMatrix() { CTX; auto& s = c->mode == LGL_PROJECTION ? c->pj : c->mode == LGL_TEXTURE ? c->tx : c->mv; if (s.size() > 1) s.pop_back(); }
static void APIENTRY shgl_Viewport(GLint x, GLint y, GLsizei w, GLsizei h) { CTX; ensure_fbo(c); glBindFramebuffer(GL_FRAMEBUFFER, c->fbo); glViewport(x, y, w, h); }

static bool* flag_for(GLCtx* c, GLenum cap)
{
	switch (cap)
	{
	case LGL_LIGHTING: return &c->lighting;
	case LGL_COLOR_MATERIAL: return &c->colmat;
	case LGL_FOG: return &c->fog;
	case GL_TEXTURE_2D: return &c->tex2d;
	case LGL_ALPHA_TEST: return &c->alphatest;
	}
	if (cap >= LGL_LIGHT0 && cap < LGL_LIGHT0 + 8) return &c->L[cap - LGL_LIGHT0].on;
	return nullptr;
}
static void APIENTRY shgl_Enable(GLenum cap)
{
	CTX; bool* f = flag_for(c, cap);
	if (f) { *f = true; return; }
	if (cap == GL_DEPTH_TEST || cap == GL_CULL_FACE || cap == GL_BLEND || cap == GL_STENCIL_TEST || cap == GL_DITHER || cap == GL_POLYGON_OFFSET_FILL || cap == GL_SCISSOR_TEST) glEnable(cap);
}
static void APIENTRY shgl_Disable(GLenum cap)
{
	CTX; bool* f = flag_for(c, cap);
	if (f) { *f = false; return; }
	if (cap == GL_DEPTH_TEST || cap == GL_CULL_FACE || cap == GL_BLEND || cap == GL_STENCIL_TEST || cap == GL_DITHER || cap == GL_POLYGON_OFFSET_FILL || cap == GL_SCISSOR_TEST) glDisable(cap);
}
static void APIENTRY shgl_Hint(GLenum t, GLenum m) {}
static void APIENTRY shgl_ShadeModel(GLenum m) { CTX; c->flat = (m == LGL_FLAT); }
static void APIENTRY shgl_PolygonMode(GLenum face, GLenum m) { CTX; c->polymode = m; }
static void APIENTRY shgl_FrontFace(GLenum m) { CTX; glFrontFace(m); }
static void APIENTRY shgl_CullFace(GLenum m) { CTX; glCullFace(m); }
static void APIENTRY shgl_DepthFunc(GLenum f) { CTX; glDepthFunc(f); }
static void APIENTRY shgl_DepthMask(GLboolean f) { CTX; glDepthMask(f); }
static void APIENTRY shgl_DepthRange(GLclampd n, GLclampd f) { CTX; glDepthRangef((float)n, (float)f); }
static void APIENTRY shgl_ColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) { CTX; glColorMask(r, g, b, a); }
static void APIENTRY shgl_BlendFunc(GLenum s, GLenum d) { CTX; glBlendFunc(s, d); }
static void APIENTRY shgl_AlphaFunc(GLenum f, GLclampf r) { CTX; c->alphafunc = f; c->alpharef = r; }
static void APIENTRY shgl_StencilOp(GLenum a, GLenum b, GLenum cc) { CTX; glStencilOp(a, b, cc); }
static void APIENTRY shgl_StencilFunc(GLenum f, GLint r, GLuint m) { CTX; glStencilFunc(f, r, m); }
static void APIENTRY shgl_StencilMask(GLuint m) { CTX; glStencilMask(m); }
static void APIENTRY shgl_ClearStencil(GLint s) { CTX; glClearStencil(s); }
static void APIENTRY shgl_ClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a) { CTX; glClearColor(r, g, b, a); }
static void APIENTRY shgl_Clear(GLbitfield m) { CTX; ensure_fbo(c); glBindFramebuffer(GL_FRAMEBUFFER, c->fbo); glClear(m & (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)); }
static void APIENTRY shgl_PixelStorei(const GLenum p, GLint v) { CTX; if (p == GL_UNPACK_ALIGNMENT || p == GL_PACK_ALIGNMENT) glPixelStorei(p, v); }
static void APIENTRY shgl_GetIntegerv(GLenum p, GLint* v)
{
	CTX;
	if (p == LGL_STENCIL_BITS) { *v = 8; return; }
	if (p == GL_MAX_TEXTURE_SIZE) { *v = 2048; return; }
	glGetIntegerv(p, v);
}

static void APIENTRY shgl_Lightfv(GLenum l, GLenum p, const GLfloat* v)
{
	CTX; if (l < LGL_LIGHT0 || l >= LGL_LIGHT0 + 8) return;
	Light& L = c->L[l - LGL_LIGHT0];
	switch (p)
	{
	case LGL_AMBIENT: memcpy(L.amb, v, 16); break;
	case LGL_DIFFUSE: memcpy(L.dif, v, 16); break;
	case LGL_SPECULAR: memcpy(L.spec, v, 16); break;
	case LGL_POSITION: xform(c->mv.back(), v, L.pos); break;
	case LGL_SPOT_DIRECTION:
	{
		const M4& m = c->mv.back();
		for (int r = 0; r < 3; r++) L.spotdir[r] = m.m[r] * v[0] + m.m[4 + r] * v[1] + m.m[8 + r] * v[2];
		break;
	}
	case LGL_SPOT_EXPONENT: L.spotexp = v[0]; break;
	case LGL_SPOT_CUTOFF: L.spotcut = v[0]; break;
	case LGL_CONSTANT_ATTENUATION: L.att[0] = v[0]; break;
	case LGL_LINEAR_ATTENUATION: L.att[1] = v[0]; break;
	case LGL_QUADRATIC_ATTENUATION: L.att[2] = v[0]; break;
	}
}
static void APIENTRY shgl_LightModelfv(GLenum p, const GLfloat* v)
{
	CTX;
	if (p == LGL_LIGHT_MODEL_AMBIENT) memcpy(c->lmamb, v, 16);
	else if (p == LGL_LIGHT_MODEL_TWO_SIDE) c->twoside = v[0] != 0;
	else if (p == LGL_LIGHT_MODEL_LOCAL_VIEWER) c->localview = v[0] != 0;
}
static void APIENTRY shgl_Materialfv(GLenum face, GLenum p, const GLfloat* v)
{
	CTX;
	switch (p)
	{
	case LGL_AMBIENT: memcpy(c->mamb, v, 16); break;
	case LGL_DIFFUSE: memcpy(c->mdif, v, 16); break;
	case LGL_AMBIENT_AND_DIFFUSE: memcpy(c->mamb, v, 16); memcpy(c->mdif, v, 16); break;
	case LGL_SPECULAR: memcpy(c->mspec, v, 16); break;
	case LGL_EMISSION: memcpy(c->memi, v, 16); break;
	case LGL_SHININESS: c->mshin = v[0]; break;
	}
}
static void APIENTRY shgl_ColorMaterial(GLenum f, GLenum m) { CTX; c->colmatMode = m; }
static void APIENTRY shgl_Fogfv(GLenum p, GLfloat* v)
{
	CTX;
	switch (p)
	{
	case LGL_FOG_MODE: c->fogmode = (GLenum)v[0]; break;
	case LGL_FOG_START: c->fogstart = v[0]; break;
	case LGL_FOG_END: c->fogend = v[0]; break;
	case LGL_FOG_DENSITY: c->fogdens = v[0]; break;
	case LGL_FOG_COLOR: memcpy(c->fogcol, v, 16); break;
	}
}
static void APIENTRY shgl_TexEnvi(GLenum t, GLenum p, GLint v) { CTX; if (p == LGL_TEXTURE_ENV_MODE) c->texenv = v; }
static void APIENTRY shgl_TexGeni(GLenum a, GLenum b, GLint v) {}
static void APIENTRY shgl_LineWidth(GLfloat w) {}

static TexRec& bound_tex(GLCtx* c)
{
	TexRec& t = c->tex[c->bound];
	if (!t.tex) glGenTextures(1, &t.tex);
	return t;
}
static void APIENTRY shgl_TexImage2D(GLenum target, GLint level, GLint comps, GLsizei w, GLsizei h, GLint border, GLenum fmt, GLenum type, const GLvoid* px)
{
	CTX;
	if (w <= 0 || h <= 0) return;
	TexRec& t = bound_tex(c);
	glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, t.tex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	GLenum f = (fmt == GL_RGB) ? GL_RGB : GL_RGBA;
	glTexImage2D(GL_TEXTURE_2D, level, f == GL_RGB ? GL_RGB8 : GL_RGBA8, w, h, 0, f, GL_UNSIGNED_BYTE, px);
	if (level == 0) { t.maxLevel = 0; t.w = w; t.h = h; }
	else if (level > t.maxLevel) t.maxLevel = level;
}
static void APIENTRY shgl_TexParameteri(GLenum target, GLenum p, GLint v)
{
	CTX; TexRec& t = c->tex[c->bound];
	if (v == LGL_CLAMP) v = GL_CLAMP_TO_EDGE;
	switch (p)
	{
	case GL_TEXTURE_MIN_FILTER: t.minf = v; break;
	case GL_TEXTURE_MAG_FILTER: t.magf = v; break;
	case GL_TEXTURE_WRAP_S: t.ws = v; break;
	case GL_TEXTURE_WRAP_T: t.wt = v; break;
	}
}
static void APIENTRY shgl_GenTextures(GLsizei n, GLuint* out)
{
	CTX; static GLuint next = 1;
	for (int i = 0; i < n; i++) { GLuint id = next++; c->tex[id]; out[i] = id; }
}
static void APIENTRY shgl_BindTexture(GLenum target, GLuint id) { CTX; c->bound = id; }
static void APIENTRY shgl_DeleteTextures(GLsizei n, const GLuint* ids)
{
	CTX;
	for (int i = 0; i < n; i++) { auto it = c->tex.find(ids[i]); if (it != c->tex.end()) { if (it->second.tex) glDeleteTextures(1, &it->second.tex); c->tex.erase(it); } if (c->bound == ids[i]) c->bound = 0; }
}

static ClientArray* arr_for(GLCtx* c, GLenum a)
{
	switch (a) { case LGL_VERTEX_ARRAY: return &c->va; case LGL_NORMAL_ARRAY: return &c->na; case LGL_COLOR_ARRAY: return &c->ca; case LGL_TEXTURE_COORD_ARRAY: return &c->ta; }
	return nullptr;
}
static void APIENTRY shgl_EnableClientState(GLenum a) { CTX; ClientArray* p = arr_for(c, a); if (p) p->on = true; }
static void APIENTRY shgl_DisableClientState(GLenum a) { CTX; ClientArray* p = arr_for(c, a); if (p) p->on = false; }
static void APIENTRY shgl_VertexPointer(GLint s, GLenum t, GLsizei st, const GLvoid* p) { CTX; c->va.size = s; c->va.type = t; c->va.stride = st; c->va.ptr = p; }
static void APIENTRY shgl_NormalPointer(GLenum t, GLsizei st, const GLvoid* p) { CTX; c->na.size = 3; c->na.type = t; c->na.stride = st; c->na.ptr = p; }
static void APIENTRY shgl_ColorPointer(GLint s, GLenum t, GLsizei st, const GLvoid* p) { CTX; c->ca.size = s; c->ca.type = t; c->ca.stride = st; c->ca.ptr = p; }
static void APIENTRY shgl_TexCoordPointer(GLint s, GLenum t, GLsizei st, const GLvoid* p) { CTX; c->ta.size = s; c->ta.type = t; c->ta.stride = st; c->ta.ptr = p; }
static void APIENTRY shgl_IndexPointer(GLenum t, GLsizei st, const GLvoid* p) {}
static void APIENTRY shgl_ArrayElement(GLint i) { CTX; emit_array_element(c, i); }
static void draw_arrays_common(GLCtx* c, GLenum mode, int count, const std::function<int(int)>& idx)
{
	float col[4], nrm[3], uv[2]; memcpy(col, c->col, 16); memcpy(nrm, c->nrm, 12); memcpy(uv, c->uv, 8);
	c->imm.clear(); c->imm.reserve(count * 13);
	for (int i = 0; i < count; i++) emit_array_element(c, idx(i));
	std::vector<float> t; GLenum m; to_triangles(c->imm, mode, t, m); draw_tris(c, t, m); c->imm.clear();
	// GL semantics: current attribs after DrawArrays are undefined; restore to keep state sane
	memcpy(c->col, col, 16); memcpy(c->nrm, nrm, 12); memcpy(c->uv, uv, 8);
}
static void APIENTRY shgl_DrawArrays(GLenum mode, GLint first, GLsizei count) { CTX; draw_arrays_common(c, mode, count, [&](int i) { return first + i; }); }
static void APIENTRY shgl_DrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid* ind)
{
	CTX;
	draw_arrays_common(c, mode, count, [&](int i) {
		if (type == GL_UNSIGNED_INT) return (int)((const GLuint*)ind)[i];
		if (type == GL_UNSIGNED_SHORT) return (int)((const GLushort*)ind)[i];
		return (int)((const GLubyte*)ind)[i];
	});
}

// ------------------------------------------------------------------ WGL
EM_JS(void, jsgl_canvas, (int id, int on), { PeterWin.glCanvas(id, on); });

static HGLRC WINAPI shwgl_CreateContext(HDC dc)
{
	DCRec* d = dc_get(dc); if (!d || !d->wnd) return NULL;
	Wnd* w = d->wnd;
	w->glCanvas = true;
	jsgl_canvas(w->id, 1);
	wnd_sync_dom(w);
	EmscriptenWebGLContextAttributes at; emscripten_webgl_init_context_attributes(&at);
	at.majorVersion = 2; at.minorVersion = 0; at.depth = true; at.stencil = true; at.antialias = false; at.alpha = false;
	at.preserveDrawingBuffer = false; at.premultipliedAlpha = false;
	char sel[64]; snprintf(sel, sizeof sel, "#peter-gl-%d", w->id);
	EMSCRIPTEN_WEBGL_CONTEXT_HANDLE h = emscripten_webgl_create_context(sel, &at);
	if (h <= 0) { SHIM_LOG("[gl] WebGL2 context creation failed (%d)", (int)h); return NULL; }
	GLCtx* c = new GLCtx; c->h = h; c->wnd = w;
	emscripten_webgl_make_context_current(h);
	build_programs(c);
	glGenVertexArrays(1, &c->vao); glBindVertexArray(c->vao);
	glGenBuffers(1, &c->vbo); glBindBuffer(GL_ARRAY_BUFFER, c->vbo);
	glEnableVertexAttribArray(0); glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 52, (void*)0);
	glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 52, (void*)16);
	glEnableVertexAttribArray(2); glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 52, (void*)28);
	glEnableVertexAttribArray(3); glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, 52, (void*)44);
	c->L[0].dif[0] = c->L[0].dif[1] = c->L[0].dif[2] = 1; c->L[0].spec[0] = c->L[0].spec[1] = c->L[0].spec[2] = 1;
	GLCtx* old = g_ctx; g_ctx = c;
	ensure_fbo(c);
	glBindFramebuffer(GL_FRAMEBUFFER, c->fbo);
	glViewport(0, 0, c->fw, c->fh);
	g_ctx = old;
	if (old) emscripten_webgl_make_context_current(old->h);
	return (HGLRC)c;
}
static WINBOOL WINAPI shwgl_MakeCurrent(HDC dc, HGLRC rc)
{
	GLCtx* c = checked<GLCtx>(rc, 0x474c4358);
	g_ctx = c; g_curDC = dc;
	if (c) emscripten_webgl_make_context_current(c->h);
	return TRUE;
}
static WINBOOL WINAPI shwgl_DeleteContext(HGLRC rc)
{
	GLCtx* c = checked<GLCtx>(rc, 0x474c4358); if (!c) return FALSE;
	if (g_ctx == c) g_ctx = nullptr;
	emscripten_webgl_destroy_context(c->h);
	if (c->wnd && c->wnd->magic == 0x574e4430) { c->wnd->glCanvas = false; jsgl_canvas(c->wnd->id, 0); }
	c->magic = 0; delete c;
	return TRUE;
}
static HDC WINAPI shwgl_GetCurrentDC() { return g_curDC; }

void gl_swap(Wnd* w)
{
	GLCtx* c = g_ctx; if (!c || c->wnd != w || !c->fbo) return;
	glBindFramebuffer(GL_READ_FRAMEBUFFER, c->fbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	GLint sc = glIsEnabled(GL_SCISSOR_TEST); if (sc) glDisable(GL_SCISSOR_TEST);
	GLboolean cm[4]; glGetBooleanv(GL_COLOR_WRITEMASK, cm); glColorMask(1, 1, 1, 1);
	glBlitFramebuffer(0, 0, c->fw, c->fh, 0, 0, c->fw, c->fh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	glColorMask(cm[0], cm[1], cm[2], cm[3]);
	if (sc) glEnable(GL_SCISSOR_TEST);
	glBindFramebuffer(GL_FRAMEBUFFER, c->fbo);
	EM_ASM({ Module.frames = (Module.frames || 0) + 1; });
}
void gl_resize(Wnd* w)
{
	RECT r = w->clientRect();
	EM_ASM({ PeterWin.glSize($0, $1, $2); }, w->id, (int)(r.right - r.left), (int)(r.bottom - r.top));
	GLCtx* c = g_ctx;
	if (c && c->wnd == w) { ensure_fbo(c); glBindFramebuffer(GL_FRAMEBUFFER, c->fbo); }
}

// ------------------------------------------------------------------ pixel formats (gdi32)
extern "C" {
int WINAPI ChoosePixelFormat(HDC dc, const PIXELFORMATDESCRIPTOR* p) { return 1; }
WINBOOL WINAPI SetPixelFormat(HDC dc, int f, const PIXELFORMATDESCRIPTOR* p) { DCRec* d = dc_get(dc); if (d && d->wnd) d->wnd->pixelformat = f; return TRUE; }
int WINAPI GetPixelFormat(HDC dc) { DCRec* d = dc_get(dc); return (d && d->wnd) ? d->wnd->pixelformat : 0; }
int WINAPI DescribePixelFormat(HDC dc, int f, UINT n, LPPIXELFORMATDESCRIPTOR p)
{
	if (p && n >= sizeof(*p))
	{
		memset(p, 0, sizeof(*p)); p->nSize = sizeof(*p); p->nVersion = 1;
		p->dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER; p->iPixelType = PFD_TYPE_RGBA;
		p->cColorBits = 32; p->cRedBits = p->cGreenBits = p->cBlueBits = p->cAlphaBits = 8; p->cDepthBits = 24; p->cStencilBits = 8;
	}
	return 1;
}
WINBOOL WINAPI SwapBuffers(HDC dc) { DCRec* d = dc_get(dc); if (d && d->wnd) gl_swap(d->wnd); return TRUE; }
}

// ------------------------------------------------------------------ name table
void* gl_get_proc(const char* name)
{
	static std::map<std::string, void*> t = {
#define E(n) {#n == std::string("x") ? "" : "gl" #n, (void*)shgl_##n}
		E(Begin), E(End), E(Vertex2i), E(Vertex2f), E(Vertex3f), E(Vertex3fv), E(Normal3fv), E(TexCoord2fv),
		E(Color3f), E(Color3fv), E(Color4f), E(Color4fv), E(MatrixMode), E(LoadIdentity), E(LoadMatrixf), E(MultMatrixf),
		E(PushMatrix), E(PopMatrix), E(Viewport), E(Enable), E(Disable), E(Hint), E(ShadeModel), E(PolygonMode), E(FrontFace),
		E(CullFace), E(DepthFunc), E(DepthMask), E(DepthRange), E(ColorMask), E(BlendFunc), E(AlphaFunc), E(StencilOp),
		E(StencilFunc), E(StencilMask), E(ClearStencil), E(ClearColor), E(Clear), E(PixelStorei), E(GetIntegerv), E(Lightfv),
		E(LightModelfv), E(Materialfv), E(ColorMaterial), E(Fogfv), E(TexEnvi), E(TexGeni), E(LineWidth), E(TexImage2D),
		E(TexParameteri), E(GenTextures), E(BindTexture), E(DeleteTextures), E(EnableClientState), E(DisableClientState),
		E(VertexPointer), E(NormalPointer), E(ColorPointer), E(TexCoordPointer), E(IndexPointer), E(ArrayElement),
		E(DrawArrays), E(DrawElements),
#undef E
		{"wglCreateContext", (void*)shwgl_CreateContext}, {"wglMakeCurrent", (void*)shwgl_MakeCurrent},
		{"wglDeleteContext", (void*)shwgl_DeleteContext}, {"wglGetCurrentDC", (void*)shwgl_GetCurrentDC},
	};
	auto it = t.find(name);
	return it == t.end() ? nullptr : it->second;
}
bool gl_attach_window(Wnd* w) { return true; }
