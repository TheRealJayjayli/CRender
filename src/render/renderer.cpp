#include "renderer.h"

namespace
{
    [[nodiscard]] float randf() noexcept
    {
        thread_local std::mt19937                          gen;
        thread_local std::uniform_real_distribution<float> dist(0.f, 1.f);
        return dist(gen);
    }

    struct processed_hit
    {
        float     emission;
        float     reflectiveness;
        glm::vec3 albedo;
        cr::ray   ray;
    };
    [[nodiscard]] processed_hit
      process_hit(const cr::ray::intersection_record &record, const cr::ray &ray)
    {
        auto out = processed_hit();

        out.emission = record.material->info.emission;
        if (record.material->info.tex.has_value())
            out.albedo = record.material->info.tex->get_uv(record.uv.x, record.uv.y);
        else
            out.albedo = record.material->info.colour;

        switch (record.material->info.type)
        {
        case cr::material::glass:
        {
            // Might not need this line of code
//            const auto dir_in         = glm::normalize(ray.direction);
//            const auto cos_view       = glm::dot(dir_in, record.normal);
//            const auto flipped_normal = cos_view > 0.0f;
//
//            auto eta    = record.material->info.ior;
//            auto normal = record.normal;
//            auto cos_in = 0.0f;
//
//            if (flipped_normal)
//            {
//                cos_in = cos_view;
//                normal = -normal;
//            }
//            else
//            {
//                cos_in = -cos_view;
//                eta    = 1.0f / eta;
//            }
//
//            const auto cos_out_sq = 1.0f - eta * eta * (1.0f - cos_in * cos_in);
//            const auto cos_out = glm::sqrt(glm::max(0.0f, cos_out_sq));
//
//            const auto f_refl = cr::sampling::frensel_reflectance(cos_in, cos_out, eta);
//
//            const auto refl = (cos_out_sq <= 0) || (::randf() < f_refl);
//
//            if (refl)
//            {
//                out.ray.origin = record.intersection_point + 0.001f * normal;
//                out.ray.direction = dir_in + 2 * cos_in * normal;
//            }
//            else
//            {
//                out.ray.origin = record.intersection_point - 0.001f * normal;
//                out.ray.direction = eta * dir_in + (eta * cos_in - cos_out) * normal;
//            }
//            out.albedo         = record.material->info.colour;
//            out.reflectiveness = record.material->info.reflectiveness;
//
//            out.ray.origin = record.intersection_point + record.normal * 0.001f;
//            out.ray.direction =
//              glm::refract(ray.direction, record.normal, record.material->info.ior);
            auto cos_hemp_dir = cr::sampling::hemp_rand();
            if (glm::dot(cos_hemp_dir, record.normal) < 0.0f) cos_hemp_dir *= -1.f;
            out.ray.origin    = record.intersection_point + record.normal * 0.0001f;
            out.ray.direction = glm::normalize(cos_hemp_dir);

            out.reflectiveness = std::fmaxf(0.f, glm::dot(record.normal, out.ray.direction));
            out.albedo = record.normal * .5f + .5f;
            break;
        }
        case cr::material::metal:
        {
            auto cos_hemp_dir = cr::sampling::hemp_rand();
            if (glm::dot(cos_hemp_dir, record.normal) < 0.0f) cos_hemp_dir *= -1.f;

            const auto out_direction = glm::normalize(cos_hemp_dir);

            /*
             * GGX BRDF
             *
             *          D(h,a) * G(v,l,a) * F(v,h,f0)
             * f(v,l) = -----------------------------
             *               4(n * v) * (n * l)
             */

            const auto H     = glm::normalize(out_direction + -ray.direction);
            const auto NdotH = glm::dot(record.normal, H);
            const auto LdotH = glm::dot(out_direction, H);
            const auto NdotV = glm::dot(record.normal, -ray.direction);
            const auto NdotL = glm::max(0.0f, glm::dot(record.normal, out_direction));

            const auto D =
              cr::sampling::cook_torrence::specular_d(NdotH, record.material->info.roughness);

            const auto G = cr::sampling::cook_torrence::specular_g(
              NdotV,
              NdotL,
              record.material->info.roughness);

            const auto F =
              cr::sampling::cook_torrence::specular_f(LdotH, record.material->info.ior);

            const auto Fr = (D * G * F) / (4 * glm::max(0.0001f, NdotV));

            const auto Fd = out.albedo * (1.0f / 3.1415f);

            const auto out_colour = (Fd) + (Fr * record.material->info.reflectiveness);
            out.albedo            = out_colour;

            out.ray.origin     = record.intersection_point + record.normal * 0.0001f;
            out.ray.origin     = record.intersection_point + record.normal * 0.0001f;
            out.ray.direction  = glm::reflect(ray.direction, record.normal);
            out.reflectiveness = 0.5;
            break;
        }

        case cr::material::smooth:
        {
            auto cos_hemp_dir = cr::sampling::hemp_rand();
            if (glm::dot(cos_hemp_dir, record.normal) < 0.0f) cos_hemp_dir *= -1.f;

            out.ray.origin    = record.intersection_point + record.normal * 0.0001f;
            out.ray.direction = glm::normalize(cos_hemp_dir);

            out.reflectiveness = std::fmaxf(0.f, glm::dot(record.normal, out.ray.direction));
            break;
        }
        }

        return out;
    }

}    // namespace

cr::renderer::renderer(
  const uint64_t                    res_x,
  const uint64_t                    res_y,
  const uint64_t                    bounces,
  std::unique_ptr<cr::thread_pool> *pool,
  std::unique_ptr<cr::scene> *      scene)
    : _camera(scene->get()->registry()->camera()), _buffer(res_x, res_y), _normals(res_x, res_y),
      _albedo(res_x, res_y), _res_x(res_x), _res_y(res_y), _max_bounces(bounces),
      _thread_pool(pool), _scene(scene), _raw_buffer(res_x * res_y * 3)
{
    _management_thread = std::thread([this]() {
        while (_run_management)
        {
            const auto tasks = _get_tasks();

            if (!tasks.empty())
            {
                _thread_pool->get()->wait_on_tasks(tasks);
                _current_sample++;
            }
            else
            {
                {
                    auto guard = std::unique_lock(_pause_mutex);
                    _pause_cond_var.notify_one();
                }
                auto guard = std::unique_lock(_start_mutex);
                _start_cond_var.wait(guard);
            }
        }
    });
}

cr::renderer::~renderer()
{
    _run_management = false;
    start();    // if we're paused, start it up again
    _management_thread.join();
}

void cr::renderer::start()
{
    _buffer.clear();
    for (auto i = 0; i < _res_x * _res_y * 3; i++) _raw_buffer[i] = 0.0f;
    _current_sample = 0;

    auto guard = std::unique_lock(_start_mutex);
    _start_cond_var.notify_all();
}

void cr::renderer::pause()
{
    _pause = true;

    auto guard = std::unique_lock(_pause_mutex);
    _pause_cond_var.wait(guard);
}

void cr::renderer::update(const std::function<void()> &update)
{
    pause();

    update();

    start();
}

void cr::renderer::set_resolution(int x, int y)
{
    _res_x = x;
    _res_y = y;

    _aspect_correction = static_cast<float>(_res_x) / static_cast<float>(_res_y);

    _buffer         = cr::image(x, y);
    _raw_buffer     = std::vector<float>(x * y * 3);
    _current_sample = 0;
}

void cr::renderer::set_max_bounces(int bounces)
{
    _max_bounces = bounces;
}

cr::image *cr::renderer::current_progress() noexcept
{
    return &_buffer;
}

cr::image *cr::renderer::current_normals() noexcept
{
    return &_normals;
}

cr::image *cr::renderer::current_albedos() noexcept
{
    return &_albedo;
}

std::vector<std::function<void()>> cr::renderer::_get_tasks()
{
    auto tasks = std::vector<std::function<void()>>();

    if (_pause)
    {
        _pause = false;
        return tasks;
    }

    tasks.reserve(_res_y);

    for (auto y = 0; y < _res_y; y++)
        tasks.emplace_back([this, y] {
            for (auto x = 0; x < _res_x; x++) this->_sample_pixel(x, y);
        });

    return std::move(tasks);
}

void cr::renderer::_sample_pixel(uint64_t x, uint64_t y)
{
    auto ray = cr::camera::get_ray(
      ((static_cast<float>(x) + ::randf()) / _res_x) * _aspect_correction,
      (static_cast<float>(y) + ::randf()) / _res_y,
      *_camera);

    auto throughput = glm::vec3(1.0f, 1.0f, 1.0f);
    auto final      = glm::vec3(0.0f, 0.0f, 0.0f);

    for (auto i = 0; i < _max_bounces; i++)
    {
        auto intersection = _scene->get()->cast_ray(ray);

        if (intersection.distance == std::numeric_limits<float>::infinity())
        {
            const auto miss_uv = glm::vec2(
              0.5f + atan2f(ray.direction.z, ray.direction.x) / (2 * 3.1415f),
              0.5f - asinf(ray.direction.y) / 3.1415f);

            const auto miss_sample = _scene->get()->sample_skybox(miss_uv.x, miss_uv.y);
            final += throughput * miss_sample;

            break;
        }
        else
        {
            const auto processed = ::process_hit(intersection, ray);

            final += throughput * processed.emission * processed.albedo;
            throughput *= processed.albedo * processed.reflectiveness;

            ray = processed.ray;
        }
    }
    // flip Y
    y = _res_y - 1 - y;

    const auto base_index = (x + y * _res_x) * 3;
    _raw_buffer[base_index + 0] += final.x;
    _raw_buffer[base_index + 1] += final.y;
    _raw_buffer[base_index + 2] += final.z;

    _buffer.set(
      x,
      y,
      glm::vec3(
        glm::pow(
          glm::clamp(_raw_buffer[base_index + 0] / float(_current_sample + 1), 0.0f, 1.0f),
          1.f / 2.2f),
        glm::pow(
          glm::clamp(_raw_buffer[base_index + 1] / float(_current_sample + 1), 0.0f, 1.0f),
          1.f / 2.2f),
        glm::pow(
          glm::clamp(_raw_buffer[base_index + 2] / float(_current_sample + 1), 0.0f, 1.0f),
          1.f / 2.2f)));
}
