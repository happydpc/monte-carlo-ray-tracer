#include "photon-mapper.hpp"

#include <iostream>
#include <iomanip>
#include <thread>
#include <atomic>

#include <glm/gtx/component_wise.hpp>

#include "../../random/random.hpp"
#include "../../common/util.hpp"
#include "../../common/work-queue.hpp"
#include "../../common/constants.hpp"
#include "../../common/format.hpp"
#include "../../material/material.hpp"
#include "../../surface/surface.hpp"
#include "../../ray/interaction.hpp"

#include "../../octree/octree.cpp"
#include "../../octree/linear-octree.cpp"

PhotonMapper::PhotonMapper(const nlohmann::json& j) : Integrator(j)
{
    bool print = true;

    const nlohmann::json& pm = j.at("photon_map");

    double caustic_factor = pm.at("caustic_factor");
    size_t photon_emissions = pm.at("emissions");

    k_nearest_photons = getOptional(pm, "k_nearest_photons", 50);
    non_caustic_reject = 1.0 / caustic_factor;
    max_radius = pm.at("max_radius");
    max_caustic_radius = pm.at("max_caustic_radius");
    max_node_data = pm.at("max_photons_per_octree_leaf");
    direct_visualization = getOptional(pm, "direct_visualization", false);
    use_shadow_photons = getOptional(pm, "use_shadow_photons", true);

    min_bounce_distance = 5.0 * max_radius;
    
    BoundingBox BB = scene.BB();

    Octree<Photon> caustic_map(BB, max_node_data);
    Octree<Photon> direct_map(BB, max_node_data);
    Octree<Photon> indirect_map(BB, max_node_data);
    Octree<ShadowPhoton> shadow_map(BB, max_node_data);

    photon_emissions = static_cast<size_t>(photon_emissions * caustic_factor);

    const size_t EPW = 100000;

    double total_add_flux = 0.0;
    for (const auto& light : scene.emissives)
    {
        total_add_flux += glm::compAdd(light->material->emittance * light->area());
    }

    struct EmissionWork
    {
        EmissionWork() : light(), num_emissions(0), photon_flux(0.0) { }
        EmissionWork(std::shared_ptr<Surface::Base> light, size_t num_emissions, const glm::dvec3& photon_flux)
            : light(light), num_emissions(num_emissions), photon_flux(photon_flux) { }

        std::shared_ptr<Surface::Base> light;
        size_t num_emissions;
        glm::dvec3 photon_flux;
    };

    std::vector<EmissionWork> work_vec;

    for (const auto& light : scene.emissives)
    {
        glm::dvec3 light_flux = light->material->emittance * light->area();
        double photon_emissions_share = glm::compAdd(light_flux) / total_add_flux;
        size_t num_light_emissions = static_cast<size_t>(photon_emissions * photon_emissions_share);
        glm::dvec3 photon_flux = light_flux / static_cast<double>(num_light_emissions);

        size_t count = 0;
        while (count != num_light_emissions)
        {
            size_t emissions = count + EPW > num_light_emissions ? num_light_emissions - count : EPW;
            work_vec.emplace_back(light, emissions, photon_flux);
            count += emissions;
        }
    }

    std::shuffle(work_vec.begin(), work_vec.end(), Random::engine);
    WorkQueue<EmissionWork> work_queue(work_vec);

    std::vector<std::unique_ptr<std::thread>> threads(Integrator::num_threads);

    direct_vecs.resize(threads.size());
    indirect_vecs.resize(threads.size());
    caustic_vecs.resize(threads.size());
    shadow_vecs.resize(threads.size());

    for (size_t thread = 0; thread < threads.size(); thread++)
    {
        threads[thread] = std::make_unique<std::thread>
        (
            [this, &work_queue, thread]()
            {
                EmissionWork work;
                while (work_queue.getWork(work))
                {
                    for (size_t i = 0; i < work.num_emissions; i++)
                    {
                        glm::dvec3 pos = (*work.light)(Random::unit(), Random::unit());
                        glm::dvec3 normal = work.light->normal(pos);
                        glm::dvec3 dir = CoordinateSystem::from(Random::cosWeightedHemiSample(), normal);

                        pos += normal * C::EPSILON;

                        emitPhoton(Ray(pos, pos + dir, scene.ior), work.photon_flux, thread);
                    }
                }
            }
        );
    }

    auto begin = std::chrono::high_resolution_clock::now();
    std::unique_ptr<std::thread> print_thread;
    if (print)
    {
        std::cout << std::endl << std::string(28, '-') << "| PHOTON MAPPING PASS |" << std::string(28, '-') << std::endl << std::endl;
        std::cout << "Total number of photon emissions from light sources: " << Format::largeNumber(photon_emissions) << std::endl << std::endl;
        print_thread = std::make_unique<std::thread>([&work_queue]()
        {
            while (!work_queue.empty())
            {
                double progress = work_queue.progress();
                std::cout << std::string("\rPhotons emitted: " + Format::progress(progress));
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            }
        });
        print_thread->join();
    }

    for (auto& thread : threads)
    {
        thread->join();
    }

    std::atomic<bool> done_constructing_octrees = false;
    auto end = std::chrono::high_resolution_clock::now();
    std::string duration = Format::timeDuration(std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count());
    if (print)
    {
        std::string info = "\rPhotons emitted in " + duration + ". Constructing octrees";
        std::cout << info;
        begin = std::chrono::high_resolution_clock::now();
            
        print_thread = std::make_unique<std::thread>([&done_constructing_octrees, info]()
        {
            std::string dots("");
            int i = 0;
            while (!done_constructing_octrees)
            {
                std::cout << "\r" + std::string(60, ' ') + info + dots;
                dots += ".";
                if (i != 0 && i % 3 == 0) dots = ".";
                i++;
                std::this_thread::sleep_for(std::chrono::milliseconds(800));
            }
        });
    }
        
    size_t num_direct_photons = 0;
    size_t num_indirect_photons = 0;
    size_t num_caustic_photons = 0;
    size_t num_shadow_photons = 0;

    // Erase elements from the vectors as they are inserted in the octree, 
    // otherwise more memory than needed is used momentarily.
    auto insertAndPop = [](auto& pvec, auto& pmap)
    {
        auto i = pvec.end();
        while (i > pvec.begin())
        {
            i--;
            pmap.insert(*i);
            i = pvec.erase(i);
        }
        pvec.clear();
    };

    for (size_t thread = 0; thread < threads.size(); thread++)
    {
        num_direct_photons += direct_vecs[thread].size();
        insertAndPop(direct_vecs[thread], direct_map);

        num_indirect_photons += indirect_vecs[thread].size();
        insertAndPop(indirect_vecs[thread], indirect_map);

        num_caustic_photons += caustic_vecs[thread].size();
        insertAndPop(caustic_vecs[thread], caustic_map);

        num_shadow_photons += shadow_vecs[thread].size();
        insertAndPop(shadow_vecs[thread], shadow_map);
    }

    // Convert octrees to linear array representation
    linear_caustic_map  = LinearOctree<Photon>(caustic_map);
    linear_direct_map   = LinearOctree<Photon>(direct_map);
    linear_indirect_map = LinearOctree<Photon>(indirect_map);
    linear_shadow_map   = LinearOctree<ShadowPhoton>(shadow_map);

    done_constructing_octrees = true;

    if (print)
    {
        print_thread->join();
        end = std::chrono::high_resolution_clock::now();
        std::string duration2 = Format::timeDuration(std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count());
        std::cout << "\rPhotons emitted in " + duration + ". Octrees constructed in " + duration2 + "." << std::endl << std::endl
                  << "Photon maps and numbers of stored photons: " << std::endl << std::endl;

        std::cout << std::right
                  << std::setw(19) << "Direct photons: "   << Format::largeNumber(num_direct_photons)   << std::endl
                  << std::setw(19) << "Indirect photons: " << Format::largeNumber(num_indirect_photons) << std::endl
                  << std::setw(19) << "Caustic photons: "  << Format::largeNumber(num_caustic_photons)  << std::endl
                  << std::setw(19) << "Shadow photons: "   << Format::largeNumber(num_shadow_photons)   << std::endl;
    }
}

void PhotonMapper::emitPhoton(const Ray& ray, const glm::dvec3& flux, size_t thread)
{
    if (ray.depth == Integrator::max_ray_depth)
    {
        Log("Bias introduced: Max ray depth reached in PhotonMap::emitPhoton()");
        return;
    }

    Intersection intersection = scene.intersect(ray);

    if (!intersection)
    {
        return;
    }

    Interaction interaction(intersection, ray);

    Ray new_ray(interaction);
    glm::dvec3 BRDF = interaction.BRDF(new_ray.direction);

    if (interaction.type == Interaction::Type::DIFFUSE)
    {
        BRDF *= C::PI;
        if (ray.depth == 0 && Random::trial(non_caustic_reject))
        {
            direct_vecs[thread].emplace_back(flux / non_caustic_reject, interaction.position, ray.direction);
            createShadowPhotons(Ray(interaction.position - interaction.normal * C::EPSILON, interaction.position + ray.direction), thread);
        }
        else if (ray.specular)
        {
            caustic_vecs[thread].emplace_back(flux, interaction.position, ray.direction);
        }
        else if (Random::trial(non_caustic_reject))
        {
            indirect_vecs[thread].emplace_back(flux / non_caustic_reject, interaction.position, ray.direction);
        }
    }
    else if (interaction.type == Interaction::Type::REFLECT && ray.depth == 0 && Random::trial(non_caustic_reject))
    {
        createShadowPhotons(Ray(interaction.position - interaction.normal * C::EPSILON, interaction.position + ray.direction), thread);
    }

    glm::dvec3 new_flux = flux * BRDF;

    // Based on slide 13 of https://cgg.mff.cuni.cz/~jaroslav/teaching/2015-npgr010/slides/11%20-%20npgr010-2015%20-%20PM.pdf
    double survive = std::min(ray.depth > min_ray_depth ? 0.9 : 1.0, glm::compMax(new_flux) / glm::compMax(flux));

    if (Random::trial(survive))
    {
        emitPhoton(new_ray, new_flux / survive, thread);
    }

    return;
}

void PhotonMapper::createShadowPhotons(const Ray& ray, size_t thread, size_t depth)
{
    if (!use_shadow_photons || depth > max_ray_depth)
    {
        return;
    }

    Intersection intersection = scene.intersect(ray);

    if (!intersection)
    {
        return;
    }

    glm::dvec3 position = ray(intersection.t); 
    if (intersection.surface->material->can_diffusely_reflect)
    {
        shadow_vecs[thread].emplace_back(position);
    }

    glm::dvec3 normal = intersection.surface->normal(position);
    if (glm::dot(normal, ray.direction) > 0.0)
    {
        normal = -normal;
    }

    glm::dvec3 pos(position - normal * C::EPSILON);
    createShadowPhotons(Ray(pos, pos + ray.direction), thread, depth + 1);
}

glm::dvec3 PhotonMapper::sampleRay(Ray ray)
{
    if (ray.depth == Integrator::max_ray_depth)
    {
        Log("Bias introduced: Max ray depth reached in PhotonMap::sampleRay()");
        return glm::dvec3(0.0);
    }

    Intersection intersection = scene.intersect(ray);

    if (!intersection)
    {
        return glm::dvec3(0.0);
    }

    double survive;
    if (absorb(ray, intersection, survive))
    {
        return glm::dvec3(0.0);
    }

    Interaction interaction(intersection, ray);

    glm::dvec3 emittance = (ray.depth == 0 || ray.specular) ? interaction.material->emittance : glm::dvec3(0.0);

    if (interaction.type != Interaction::Type::DIFFUSE)
    {
        // Ray originated from diffuse reflection
        if (ray.depth != 0 && !ray.specular) return emittance / survive;

        Ray new_ray(interaction);
        return (emittance + sampleRay(new_ray) * interaction.BRDF(new_ray.direction)) / survive;
    }
    else
    {
        glm::dvec3 caustics = estimateCausticRadiance(interaction);

        auto evaluateDirect = [&]()
        {
            if (use_shadow_photons && hasShadowPhotons(interaction) && linear_direct_map.radiusEmpty(interaction.position, max_radius))
                return glm::dvec3(0.0);
            else
                return Integrator::sampleDirect(interaction);
        };

        auto evaluateDiffuse = [&]()
        {
            Ray new_ray(interaction);
            glm::dvec3 BRDF = interaction.BRDF(new_ray.direction);
            glm::dvec3 indirect = sampleRay(new_ray) * C::PI;
            return (emittance + caustics + (evaluateDirect() + indirect) * BRDF) / survive;
        };

        if (!direct_visualization && (ray.depth == 0 || ray.specular || interaction.t >= min_bounce_distance))
        {
            return evaluateDiffuse();
        }
        else
        {
            auto indirect_photons = linear_indirect_map.knnSearch(interaction.position, k_nearest_photons, max_radius);
            if (indirect_photons.size() == k_nearest_photons || direct_visualization)
            {
                glm::dvec3 direct(0.0);
                auto direct_photons = linear_direct_map.knnSearch(interaction.position, k_nearest_photons, max_radius);
                if (!direct_photons.empty())
                {
                    direct = estimateRadiance(interaction, direct_photons);
                }
                else if (!direct_visualization && use_shadow_photons && !hasShadowPhotons(interaction))
                {
                    indirect_photons.clear();
                    return evaluateDiffuse();
                }
                glm::dvec3 indirect = estimateRadiance(interaction, indirect_photons);
                return (emittance + caustics + direct + indirect) / survive;
            }
            else
            {
                indirect_photons.clear();
                return evaluateDiffuse();
            }
        }
    }
}

glm::dvec3 PhotonMapper::estimateRadiance(const Interaction& interaction, const std::vector<SearchResult<Photon>> &photons)
{
    glm::dvec3 radiance(0.0);
    if (photons.empty()) return radiance;
    for (const auto& p : photons)
    {
        if (glm::dot(p.data.direction, interaction.cs.normal) >= 0.0) continue;
        radiance += p.data.flux * interaction.BRDF(p.data.direction);
    }
    return radiance / photons.back().distance2;
}

/*********************************************************************************
Cone filtering method that can be used for sharper caustics. Simplified for k = 1
**********************************************************************************/
glm::dvec3 PhotonMapper::estimateCausticRadiance(const Interaction& interaction)
{
    glm::dvec3 radiance(0.0);
    auto photons = linear_caustic_map.knnSearch(interaction.position, k_nearest_photons, max_caustic_radius);
    if (photons.empty()) return radiance;

    double inv_max_squared_radius = 1.0 / photons.back().distance2;

    for (const auto& p : photons)
    {
        if (glm::dot(p.data.direction, interaction.cs.normal) >= 0.0) continue;
        double wp = std::max(0.0, 1.0 - std::sqrt(p.distance2 * inv_max_squared_radius));
        radiance += p.data.flux * interaction.BRDF(p.data.direction) * wp;
    }
    return 3.0 * radiance * inv_max_squared_radius;
}

bool PhotonMapper::hasShadowPhotons(const Interaction& interaction) const
{
    return !linear_shadow_map.radiusEmpty(interaction.position, max_radius);
}
