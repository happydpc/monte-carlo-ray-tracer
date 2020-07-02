#include "ray.hpp"

#include "../common/util.hpp"
#include "../random/random.hpp"
#include "../common/constants.hpp"
#include "interaction.hpp"

Ray::Ray() : start(), direction(), medium_ior(1) { }

Ray::Ray(const glm::dvec3& start) : start(start), direction(), medium_ior(1) { }

Ray::Ray(const glm::dvec3& start, const glm::dvec3& end, double medium_ior)
    : start(start), direction(glm::normalize(end - start)), medium_ior(medium_ior) { }

glm::dvec3 Ray:: operator()(double t) const
{
    return start + direction * t;
}

void Ray::reflectDiffuse(const CoordinateSystem &cs, const Interaction &ia, double n1)
{
    direction = cs.localToGlobal(Random::CosWeightedHemiSample());
    start += ia.normal * C::EPSILON;
    specular = false;
    medium_ior = n1;
}

bool Ray::reflectSpecular(const glm::dvec3 &in, const Interaction &ia, double n1)
{
    direction = glm::reflect(in, ia.specular_normal);
    start += ia.normal * C::EPSILON;
    specular = true;
    medium_ior = n1;

    return glm::dot(ia.shading_normal, direction) > 0.0;
}

bool Ray::refractSpecular(const glm::dvec3 &in, const Interaction &ia, double n1, double n2)
{
    specular = true;

    double ior_quotient = n1 / n2;
    double cos_theta = glm::dot(ia.specular_normal, in);
    double k = 1.0 - pow2(ior_quotient) * (1.0 - pow2(cos_theta)); // 1 - (n1/n2 * sin(theta))^2
    if (k >= 0)
    {
        /* SPECULAR REFRACTION */
        direction = ior_quotient * in - (ior_quotient * cos_theta + std::sqrt(k)) * ia.specular_normal;
        start -= ia.normal * C::EPSILON;
        medium_ior = n2;

        return glm::dot(ia.shading_normal, direction) < 0.0;
    }
    else
    {
        /* CRITICAL ANGLE, SPECULAR REFLECTION */
        direction = in - ia.specular_normal * cos_theta * 2.0;
        start += ia.normal * C::EPSILON;
        medium_ior = n1;

        return glm::dot(ia.shading_normal, direction) > 0.0;
    }
}